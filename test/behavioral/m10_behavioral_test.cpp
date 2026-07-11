/*
 * m10_behavioral_test.cpp — M10: external introspection (R5, POSIX-shm
 * reach). When "the controller isn't getting plans" happens on a robot,
 * diagnosis must not require rebuilding, instrumenting, or even
 * cooperating application code.
 *
 * The M1-shaped pair runs as real processes (fork + exec shm_test_helper);
 * a third observer attaches through detail/introspect_reader.hpp — and
 * through the actual `xmmsg` CLI binary run as a subprocess, because the
 * CLI is the deliverable: its stdout is what a field diagnosis reads.
 *
 * Acceptance criteria (docs/scenarios.md):
 *   A1 — live topics + endpoints enumerated with types (schema hash), QoS,
 *        and owning PIDs, with zero cooperation from the observed
 *        processes (discovery = /dev/shm scan + name grammar, §6.4/§8).
 *   A2 — externally read counters reconcile EXACTLY with the observed
 *        process's own introspect::/messaging.* view (same shared atomics).
 *   A3 — each injected fault is diagnosable from the CLI output alone:
 *        paused publisher -> rising last-publish age (pid alive);
 *        stalled consumer -> growing drop count;
 *        killed publisher -> pid marked DEAD, never silently vanished.
 *   A4 — an aggressively polling observer is invisible to the observed
 *        pair's hop-latency profile (reported numbers; generous 3x gate —
 *        CI variance makes a tight gate dishonest).
 *   A5 — reading is safe against a publisher SIGKILLed mid-store: the
 *        bounded-retry read never blocks, crashes, or returns torn data.
 *
 * Test conditions stated per family rule: inter-process, 144-byte
 * checksummed POD, latest-only QoS, cadences per test below. TSan cannot
 * see cross-process shm races (shm_test_support.hpp); the suite also runs
 * under ASan.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "shm_test_support.hpp"
#include "xmmessaging/messaging.hpp"

#if !defined(XMMESSAGING_HAS_POSIX_SHM)

TEST(M10Behavioral, DISABLED_RequiresPosixShmBackend) {
  GTEST_SKIP() << "XMMESSAGING_WITH_POSIX_SHM is off";
}

#else

#include "xmmessaging/detail/introspect_reader.hpp"

namespace msg = xmotion::messaging;
namespace det = xmotion::messaging::detail;
using namespace std::chrono_literals;

namespace {

constexpr const char* kTopic = "m10.plan.head";

std::string HashHex(std::uint64_t hash) {
  char buffer[19];
  std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                static_cast<unsigned long long>(hash));
  return buffer;
}

// Open the reader on <key, topic>; fails the calling test on any status
// but kOk.
det::IntrospectReader OpenReader(const std::string& key, const char* topic) {
  det::IntrospectReader reader;
  const auto status =
      det::IntrospectReader::Open(det::ShmSegmentName(key, topic), &reader);
  EXPECT_EQ(status, det::IntrospectOpenStatus::kOk);
  return reader;
}

// Last-publish age in microseconds from a snapshot (§8 read protocol).
double SnapshotAgeUs(const det::IntrospectSnapshot& snap) {
  const std::int64_t now_ns = xmotion::Now().time_since_epoch().count();
  return static_cast<double>(now_ns - snap.last_publish_stamp_ns) / 1000.0;
}

// Extract the first numeric value following `"key":` in a JSON string —
// enough to assert CLI-reported quantities without a full parser.
bool JsonNumber(const std::string& json, const std::string& key,
                double* value) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t at = json.find(needle);
  if (at == std::string::npos) {
    return false;
  }
  *value = std::strtod(json.c_str() + at + needle.size(), nullptr);
  return true;
}

template <typename Pred>
msg::Sample<ShmTestPlan> TakeUntil(msg::Subscriber<ShmTestPlan>& sub,
                                   msg::Duration bound, Pred pred) {
  const auto deadline = xmotion::Now() + bound;
  for (;;) {
    auto sample = sub.TakeLatest();
    if (pred(sample) || xmotion::Now() >= deadline) {
      return sample;
    }
    std::this_thread::sleep_for(2ms);
  }
}

#if defined(XMMSG_CLI_PATH)
// Run the real CLI binary; returns its stdout, asserts exit 0 and (for
// --json invocations) that the output parses.
std::string RunCli(const std::string& args, bool expect_json) {
  std::string out;
  const int code =
      shmtest::RunCommandCaptureStdout(std::string(XMMSG_CLI_PATH) + " " + args,
                                       &out);
  EXPECT_EQ(code, 0) << "xmmsg " << args << " failed:\n" << out;
  if (expect_json) {
    EXPECT_TRUE(shmtest::JsonChecker::Parses(out))
        << "xmmsg " << args << " emitted unparseable JSON:\n"
        << out;
  }
  return out;
}
#endif  // XMMSG_CLI_PATH

}  // namespace

// A1 + A2: enumeration with zero cooperation; exact counter reconciliation.
TEST(M10Behavioral, ExternalEnumerationAndReconciliation) {
  const std::string domain_name = shmtest::UniqueDomainName("m10a");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly()});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  shmtest::ChildGuard planner(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1", "1000000", "2000"}));
  ASSERT_GT(planner.pid(), 0);
  ASSERT_EQ(TakeUntil(sub, 5s,
                      [](const msg::Sample<ShmTestPlan>& s) {
                        return s.freshness() == msg::Freshness::kFresh;
                      })
                .freshness(),
            msg::Freshness::kFresh);

  // Consume for a while so the counters have content to reconcile.
  for (int i = 0; i < 50; ++i) {
    sub.TakeLatest();
    std::this_thread::sleep_for(4ms);
  }

  // A1: discovery is the /dev/shm scan + name grammar — no cooperation, no
  // registry, no query to the observed processes.
  std::vector<det::DiscoveredSegment> mine;
  for (const det::DiscoveredSegment& seg : det::DiscoverXmmsgSegments()) {
    if (seg.isolation_key == key) {
      mine.push_back(seg);
    }
  }
  ASSERT_EQ(mine.size(), 1u) << "exactly this domain's one topic";
  EXPECT_EQ(mine[0].topic, kTopic);
  EXPECT_FALSE(mine[0].hashed_name);

  det::IntrospectReader reader = OpenReader(key, kTopic);
  det::IntrospectSnapshot snap;
  ASSERT_TRUE(reader.Snapshot(&snap));

  // A1: type identity, QoS, payload shape, owning pids.
  EXPECT_EQ(snap.schema_hash, det::SchemaHashOf<ShmTestPlan>());
  EXPECT_EQ(snap.payload_size, sizeof(ShmTestPlan));
  EXPECT_EQ(snap.payload_align, alignof(ShmTestPlan));
  EXPECT_EQ(snap.creator_history_kind, 0u);  // latest-only
  EXPECT_EQ(snap.pub_pid, static_cast<std::uint32_t>(planner.pid()));
  EXPECT_TRUE(snap.pub_alive);
  ASSERT_EQ(snap.active_subscriber_count, 1u);
  EXPECT_EQ(snap.subscribers[0].pid, static_cast<std::uint32_t>(::getpid()));
  EXPECT_TRUE(snap.subscribers[0].alive);
  EXPECT_EQ(snap.master, det::MasterReadResult::kValue);
  EXPECT_GT(snap.last_ordinal, 0u);

  // A2: the externally read counters ARE the shared atomics the observed
  // process's own introspect:: verbs read — reconciliation is exact, not
  // approximate. (This subscriber stopped taking, so its counters are
  // quiescent; the publisher's keep moving and are bounded-checked.)
  const det::IntrospectSubSlot& me = snap.subscribers[0];
  EXPECT_EQ(me.take_count, msg::introspect::TakeCount(sub));
  EXPECT_EQ(me.overwrite_count, msg::introspect::OverwriteCount(sub));
  EXPECT_EQ(me.drop_count, msg::introspect::DropCount(sub));
  EXPECT_EQ(me.drop_count, 0u);  // latest-only never drops, it overwrites
  EXPECT_GT(me.take_count, 0u);
  // Publisher-side counters move between their two loads (live stream):
  // ordinal and publish count track each other within the in-flight window.
  EXPECT_GT(snap.pub_publish_count, 0u);
  EXPECT_GE(snap.accepted_ordinal, snap.pub_publish_count);
  EXPECT_LE(snap.accepted_ordinal - snap.pub_publish_count, 4u);

#if defined(XMMSG_CLI_PATH)
  // The CLI leg of A1: the deliverable enumerates the same facts.
  const std::string json = RunCli("list --domain " + key + " --json", true);
  EXPECT_NE(json.find(std::string("\"topic\":\"") + kTopic + "\""),
            std::string::npos)
      << json;
  EXPECT_NE(json.find(HashHex(snap.schema_hash)), std::string::npos) << json;
  EXPECT_NE(json.find("\"alive\":true"), std::string::npos) << json;
  const std::string plain = RunCli("list --domain " + key, false);
  EXPECT_NE(plain.find(kTopic), std::string::npos) << plain;
  EXPECT_NE(plain.find("latest-only"), std::string::npos) << plain;
#endif

  planner.Kill();
}

// A3, fault 1: paused publisher — rising last-publish age with a LIVE pid
// (the "planner stopped planning" incident, distinct from death).
TEST(M10Behavioral, PausedPublisherDiagnosable) {
  const std::string domain_name = shmtest::UniqueDomainName("m10b");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly(), .deadline = 250ms});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // 50 plans at 2 ms, a 1.5 s pause (alive, silent), 50 more.
  shmtest::ChildGuard planner(shmtest::SpawnHelper(
      {"publish_pause", domain_name, kTopic, "50", "2000", "1500", "50"}));
  ASSERT_GT(planner.pid(), 0);

  // Wait until the pause begins: the last phase-1 plan is id 50.
  auto last = TakeUntil(sub, 5s, [](const msg::Sample<ShmTestPlan>& s) {
    return s.freshness() != msg::Freshness::kNone && (*s).plan_id >= 50;
  });
  ASSERT_NE(last.freshness(), msg::Freshness::kNone);
  ASSERT_EQ((*last).plan_id, 50u);

  det::IntrospectReader reader = OpenReader(key, kTopic);
  det::IntrospectSnapshot snap1;
  ASSERT_TRUE(reader.Snapshot(&snap1));
  ASSERT_EQ(snap1.master, det::MasterReadResult::kValue);
  const double age1_us = SnapshotAgeUs(snap1);
  std::this_thread::sleep_for(400ms);
  det::IntrospectSnapshot snap2;
  ASSERT_TRUE(reader.Snapshot(&snap2));
  const double age2_us = SnapshotAgeUs(snap2);

  EXPECT_GT(age2_us, age1_us) << "last-publish age must rise while paused";
  EXPECT_GT(age2_us, 350'000.0);
  EXPECT_TRUE(snap2.pub_alive) << "paused is NOT dead — distinct diagnoses";
  EXPECT_EQ(snap2.last_ordinal, snap1.last_ordinal) << "nothing published";
  // The observed process's own deadline verdict agrees (D3): one miss.
  EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kStale);
  det::IntrospectSnapshot snap3;
  ASSERT_TRUE(reader.Snapshot(&snap3));
  EXPECT_GE(snap3.subscribers[0].deadline_miss_count, 1u);

#if defined(XMMSG_CLI_PATH)
  // A3's actual bar: the fault is visible from the CLI output ALONE.
  const std::string json =
      RunCli("stat " + std::string(kTopic) + " --domain " + key + " --json",
             true);
  double cli_age_us = 0.0;
  ASSERT_TRUE(JsonNumber(json, "age_us", &cli_age_us)) << json;
  EXPECT_GT(cli_age_us, 350'000.0) << json;
  EXPECT_NE(json.find("\"alive\":true"), std::string::npos) << json;
  double miss = 0.0;
  ASSERT_TRUE(JsonNumber(json, "deadline_miss_count", &miss)) << json;
  EXPECT_GE(miss, 1.0) << json;
#endif

  EXPECT_EQ(planner.Reap(), 0);  // publishes its second burst and exits
}

// A3, fault 2: stalled consumer — its queue overflows and the drop counter
// grows, externally visible while the flood is still running.
TEST(M10Behavioral, StalledConsumerDiagnosable) {
  const std::string domain_name = shmtest::UniqueDomainName("m10c");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  // The stalled consumer: a bounded queue it never drains.
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::Queue(8)});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  shmtest::ChildGuard flooder(shmtest::SpawnHelper(
      {"flood_queue", domain_name, kTopic, "400000"}));
  ASSERT_GT(flooder.pid(), 0);

  // Drops must be observed GROWING mid-flood (not just nonzero after).
  det::IntrospectReader reader = OpenReader(key, kTopic);
  std::uint64_t first_drops = 0;
  std::uint64_t second_drops = 0;
  const auto deadline = xmotion::Now() + 10s;
  while (xmotion::Now() < deadline) {
    det::IntrospectSnapshot snap;
    ASSERT_TRUE(reader.Snapshot(&snap));
    if (snap.active_subscriber_count == 1) {
      const std::uint64_t drops = snap.subscribers[0].drop_count;
      if (first_drops == 0) {
        first_drops = drops;
      } else if (drops > first_drops) {
        second_drops = drops;
        break;
      }
    }
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_GT(first_drops, 0u) << "the stall must surface as counted drops";
  EXPECT_GT(second_drops, first_drops) << "and they must be seen GROWING";

  EXPECT_EQ(flooder.Reap(), 0);

  // Reconciliation at quiescence (A2 for the drop counter).
  det::IntrospectSnapshot final_snap;
  ASSERT_TRUE(reader.Snapshot(&final_snap));
  EXPECT_EQ(final_snap.subscribers[0].drop_count,
            msg::introspect::DropCount(sub));

#if defined(XMMSG_CLI_PATH)
  const std::string json =
      RunCli("stat " + std::string(kTopic) + " --domain " + key + " --json",
             true);
  double cli_drops = 0.0;
  ASSERT_TRUE(JsonNumber(json, "drop_count", &cli_drops)) << json;
  EXPECT_GT(cli_drops, 0.0) << json;
#endif
}

// A3, fault 3: killed publisher — the dead endpoint is MARKED (pid shown,
// liveness false), never silently vanished. Reuses the M4 kill machinery.
TEST(M10Behavioral, KilledPublisherDiagnosable) {
  const std::string domain_name = shmtest::UniqueDomainName("m10d");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly()});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  shmtest::ChildGuard planner(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1", "1000000", "2000"}));
  ASSERT_GT(planner.pid(), 0);
  ASSERT_EQ(TakeUntil(sub, 5s,
                      [](const msg::Sample<ShmTestPlan>& s) {
                        return s.freshness() == msg::Freshness::kFresh;
                      })
                .freshness(),
            msg::Freshness::kFresh);
  const pid_t dead_pid = planner.pid();
  ASSERT_EQ(planner.Kill(), -SIGKILL);

  det::IntrospectReader reader = OpenReader(key, kTopic);
  det::IntrospectSnapshot snap;
  ASSERT_TRUE(reader.Snapshot(&snap));
  EXPECT_EQ(snap.pub_pid, static_cast<std::uint32_t>(dead_pid))
      << "the dead endpoint must stay listed, not vanish";
  EXPECT_FALSE(snap.pub_alive);
  EXPECT_GE(snap.pub_epoch, 1u);

#if defined(XMMSG_CLI_PATH)
  const std::string plain = RunCli("list --domain " + key, false);
  EXPECT_NE(plain.find("DEAD"), std::string::npos) << plain;
  EXPECT_NE(plain.find(std::to_string(dead_pid)), std::string::npos) << plain;
  const std::string json =
      RunCli("stat " + std::string(kTopic) + " --domain " + key + " --json",
             true);
  EXPECT_NE(json.find("\"alive\":false"), std::string::npos) << json;
#endif
}

// A4: attaching + aggressively polling the observer must be invisible to
// the observed pair's latency profile. Gate is deliberately generous (3x,
// CI variance); the NUMBERS are the deliverable and are always printed.
TEST(M10Behavioral, ObserverInvisibleToLatency) {
  const std::string domain_name = shmtest::UniqueDomainName("m10e");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly()});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // 1 kHz publisher, long enough to cover both measurement phases.
  shmtest::ChildGuard planner(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1", "1000000", "1000"}));
  ASSERT_GT(planner.pid(), 0);
  ASSERT_EQ(TakeUntil(sub, 5s,
                      [](const msg::Sample<ShmTestPlan>& s) {
                        return s.freshness() == msg::Freshness::kFresh;
                      })
                .freshness(),
            msg::Freshness::kFresh);

  // Hop latency proxy: age at first sight of each new plan (the consumer
  // polls at ~100 us, so poll quantization dominates — identical in both
  // phases, which is what makes the A/B honest).
  const auto measure = [&sub](int samples) {
    std::vector<double> ages_us;
    ages_us.reserve(samples);
    std::uint64_t last_id = 0;
    const auto bound = xmotion::Now() + 10s;
    while (static_cast<int>(ages_us.size()) < samples &&
           xmotion::Now() < bound) {
      auto sample = sub.TakeLatest();
      if (sample.freshness() == msg::Freshness::kFresh &&
          (*sample).plan_id != last_id) {
        last_id = (*sample).plan_id;
        ages_us.push_back(
            std::chrono::duration<double, std::micro>(sample.age()).count());
      }
      std::this_thread::sleep_for(100us);
    }
    std::sort(ages_us.begin(), ages_us.end());
    return ages_us;
  };
  const auto p = [](const std::vector<double>& sorted, double q) {
    return sorted.empty()
               ? 0.0
               : sorted[static_cast<std::size_t>(
                     q * static_cast<double>(sorted.size() - 1))];
  };

  // Phase 1: no observer.
  const std::vector<double> baseline = measure(400);
  ASSERT_GE(baseline.size(), 300u);

  // Phase 2: an aggressive observer — full open+snapshot cycles in a tight
  // loop (worse than any real CLI cadence; `watch` polls at 1 Hz).
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observer_snapshots{0};
  std::thread observer([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      det::IntrospectReader reader;
      if (det::IntrospectReader::Open(det::ShmSegmentName(key, kTopic),
                                      &reader) ==
          det::IntrospectOpenStatus::kOk) {
        det::IntrospectSnapshot snap;
        reader.Snapshot(&snap);
        observer_snapshots.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  const std::vector<double> observed = measure(400);
  stop.store(true, std::memory_order_relaxed);
  observer.join();
  ASSERT_GE(observed.size(), 300u);
  EXPECT_GT(observer_snapshots.load(), 100u)
      << "the observer must actually have been aggressive";

  const double base_p50 = p(baseline, 0.50);
  const double base_p99 = p(baseline, 0.99);
  const double obs_p50 = p(observed, 0.50);
  const double obs_p99 = p(observed, 0.99);
  // The report IS the deliverable; the gate is a generous tripwire.
  std::printf(
      "[M10-A4] hop-age proxy, 1 kHz publisher, 100 us consumer poll\n"
      "[M10-A4]   baseline: p50 %.1f us  p99 %.1f us  (n=%zu)\n"
      "[M10-A4]   observed: p50 %.1f us  p99 %.1f us  (n=%zu, %llu observer "
      "snapshots)\n",
      base_p50, base_p99, baseline.size(), obs_p50, obs_p99, observed.size(),
      static_cast<unsigned long long>(observer_snapshots.load()));
  ::testing::Test::RecordProperty("m10_a4_baseline_p99_us",
                                  std::to_string(base_p99));
  ::testing::Test::RecordProperty("m10_a4_observed_p99_us",
                                  std::to_string(obs_p99));
  EXPECT_LE(obs_p99, std::max(3.0 * base_p99, base_p99 + 500.0))
      << "observer polling must not shift the observed pair's tail";

  planner.Kill();
}

// A5: the observer survives a publisher SIGKILLed mid-store — bounded
// retries mean every snapshot returns promptly, torn reads are impossible
// (seqlock validation), and a permanently-odd sequence reads as kStalled.
TEST(M10Behavioral, ObserverSafeAcrossPublisherKill) {
  const std::string domain_name = shmtest::UniqueDomainName("m10f");
  const std::string key = det::DeriveIsolationKey(domain_name);
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly()});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  for (int generation = 0; generation < 3; ++generation) {
    // Full-rate publisher (no inter-publish sleep): maximizes the chance
    // the SIGKILL lands mid-seqlock-store.
    shmtest::ChildGuard planner(shmtest::SpawnHelper(
        {"publish_stream", domain_name, kTopic, "1", "1000000000", "0"}));
    ASSERT_GT(planner.pid(), 0);

    det::IntrospectReader reader = OpenReader(key, kTopic);
    // Kill from a second thread while the main thread reads continuously,
    // so the kill genuinely races active snapshots.
    std::thread killer([&planner] {
      std::this_thread::sleep_for(50ms);
      planner.Kill();
    });
    const auto read_until = xmotion::Now() + 300ms;
    std::uint64_t snapshots = 0;
    msg::Duration worst = msg::Duration::zero();
    det::IntrospectSnapshot snap;
    while (xmotion::Now() < read_until) {
      const auto t0 = xmotion::Now();
      ASSERT_TRUE(reader.Snapshot(&snap));  // must return, never hang
      const auto took = xmotion::Now() - t0;
      if (took > worst) {
        worst = took;
      }
      ++snapshots;
    }
    killer.join();
    EXPECT_GT(snapshots, 100u);
    EXPECT_LT(worst, 100ms) << "bounded-retry reads must stay bounded";
    // After the kill: the endpoint is marked dead; the master read is
    // either a validated value or an honest kStalled — never a hang, never
    // garbage (the seqlock rejects torn copies by construction).
    ASSERT_TRUE(reader.Snapshot(&snap));
    EXPECT_FALSE(snap.pub_alive);
    if (snap.master == det::MasterReadResult::kValue) {
      EXPECT_GT(snap.last_ordinal, 0u);
      EXPECT_LE(snap.last_ordinal, snap.accepted_ordinal);
    }
  }
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

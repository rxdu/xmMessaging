/*
 * m4_behavioral_test.cpp — M4: crash of one process, recovery (P1b,
 * POSIX-shm reach — the daemonless argument made falsifiable).
 *
 * Asserts the M4 acceptance criteria from docs/scenarios.md against the
 * POSIX shm fallback backend, with a REAL SIGKILL of a real child process:
 *   A1 — the consumer never blocks, crashes, or takes a torn value across
 *        the kill; it observes rising staleness and its deadline flag.
 *   A2 — the restarted publisher re-advertises the SAME topic successfully
 *        (dead-owner reclaim of the segment's publisher-liveness slot;
 *        ordinals resume from the segment's accepted_ordinal).
 *   A3 — the incumbent subscriber receives fresh values after rejoin with
 *        zero re-wiring code — the subscription outlives the peer.
 *   A4 — death and rejoin are observable events: MatchedCount() drops to 0
 *        on death and returns to 1 on rejoin (the wiring-path liveness
 *        probe), and the segment's publisher epoch increments.
 *
 * Test conditions stated per family rule: inter-process (fork + exec of
 * shm_test_helper), 2 ms publish period, 250 ms deadline, 144-byte
 * checksummed POD. TSan cannot see cross-process races (noted in
 * shm_test_support.hpp); this suite also runs under ASan, which does catch
 * mapping/bounds bugs.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "shm_test_support.hpp"
#include "xmmsg/messaging.hpp"

#if !defined(XMMESSAGING_HAS_POSIX_SHM)

TEST(M4Behavioral, DISABLED_RequiresPosixShmBackend) {
  GTEST_SKIP() << "XMMESSAGING_WITH_POSIX_SHM is off";
}

#else

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

constexpr const char* kTopic = "m4.plan.head";

// Poll a take until the predicate holds or the bound passes; returns the
// last sample. The consumer loop owns its cadence (D5) — this is the test's
// stand-in for a control loop.
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

}  // namespace

TEST(M4Behavioral, CrashRecovery_KillRestartRejoin) {
  const std::string domain_name = shmtest::UniqueDomainName("m4");
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  // The control side: wired ONCE, before any publisher exists, and never
  // re-wired for the rest of the test (A3's precondition).
  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly(), .deadline = 250ms});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);
  EXPECT_EQ(sub.MatchedCount(), 0u);  // no publisher yet

  // ---- planner generation 1 ------------------------------------------------
  shmtest::ChildGuard planner1(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1", "1000000", "2000"}));
  ASSERT_GT(planner1.pid(), 0);

  auto first = TakeUntil(sub, 5s, [](const msg::Sample<ShmTestPlan>& s) {
    return s.freshness() == msg::Freshness::kFresh;
  });
  ASSERT_EQ(first.freshness(), msg::Freshness::kFresh)
      << "planner child never delivered a value";
  ASSERT_TRUE(ShmPlanConsistent(*first));
  EXPECT_EQ(sub.MatchedCount(), 1u);

  // ---- SIGKILL mid-stream (A1/A4) -------------------------------------------
  const int killed = planner1.Kill();
  ASSERT_EQ(killed, -SIGKILL) << "planner was expected to die by SIGKILL";

  // A1: across and after the kill, every take stays untorn and non-blocking;
  // the value freezes at the last published plan and its staleness rises.
  std::uint64_t frozen_id = 0;
  msg::Duration last_age = msg::Duration::zero();
  bool age_monotonic = true;
  bool ever_torn = false;
  const auto watch_deadline = xmotion::Now() + 600ms;
  while (xmotion::Now() < watch_deadline) {
    auto sample = sub.TakeLatest();
    ASSERT_NE(sample.freshness(), msg::Freshness::kNone)
        << "a received value must never regress to kNone (M4-A1)";
    if (!ShmPlanConsistent(*sample)) {
      ever_torn = true;
    }
    if (frozen_id == 0) {
      frozen_id = (*sample).plan_id;  // whatever was newest at death
    } else {
      EXPECT_EQ((*sample).plan_id, frozen_id)
          << "no phantom values after the publisher died";
    }
    if (sample.age() < last_age) {
      age_monotonic = false;
    }
    last_age = sample.age();
    std::this_thread::sleep_for(25ms);
  }
  EXPECT_FALSE(ever_torn) << "torn value observed across a SIGKILL (M4-A1)";
  EXPECT_TRUE(age_monotonic) << "staleness must rise monotonically";
  EXPECT_GT(last_age, 250ms);

  // The deadline flag raised (D2/D3): the frozen value is now kStale.
  auto stale = sub.TakeLatest();
  EXPECT_EQ(stale.freshness(), msg::Freshness::kStale);
  EXPECT_EQ((*stale).plan_id, frozen_id);

  // A4: the death is an observable event, not just inferable staleness.
  EXPECT_EQ(sub.MatchedCount(), 0u);

  // ---- planner generation 2 (A2/A3) -----------------------------------------
  // Same topic, same segment; ids offset so rejoin values are unmistakable.
  // If the dead publisher's slot were NOT reclaimable, the helper would
  // exit(2) on the refused Advertise and no fresh value could ever arrive.
  shmtest::ChildGuard planner2(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "2000000", "1000000", "2000"}));
  ASSERT_GT(planner2.pid(), 0);

  auto rejoined =
      TakeUntil(sub, 5s, [](const msg::Sample<ShmTestPlan>& s) {
        return s.freshness() == msg::Freshness::kFresh &&
               (*s).plan_id >= 2000000;
      });
  ASSERT_EQ(rejoined.freshness(), msg::Freshness::kFresh)
      << "no fresh value after publisher rejoin (M4-A2/A3)";
  EXPECT_GE((*rejoined).plan_id, 2000000u);
  EXPECT_TRUE(ShmPlanConsistent(*rejoined));
  EXPECT_EQ(sub.MatchedCount(), 1u);  // A4: rejoin observable too

  planner2.Kill();
}

// A2 in isolation, plus ordinal continuity: the restarted publisher resumes
// the segment's ordinal sequence, so per-subscriber conservation accounting
// stays exact ACROSS the crash (delivered-once + overwritten == published,
// summed over both publisher generations).
TEST(M4Behavioral, OrdinalContinuityAcrossRestart) {
  const std::string domain_name = shmtest::UniqueDomainName("m4ord");
  shmtest::SegmentJanitor janitor(domain_name, {kTopic});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      kTopic, {.history = msg::History::LatestOnly()});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // Generation 1: publishes exactly 50 values, exits cleanly.
  shmtest::ChildGuard gen1(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1", "50", "200"}));
  ASSERT_EQ(gen1.Reap(), 0);
  // Generation 2: 50 more, ids disjoint.
  shmtest::ChildGuard gen2(shmtest::SpawnHelper(
      {"publish_stream", domain_name, kTopic, "1000", "50", "200"}));
  ASSERT_EQ(gen2.Reap(), 0);

  // Drain: the subscriber consumed nothing while both ran, so the newest
  // value is generation 2's last, and the gap accounting must charge all
  // 99 unseen values — 100 published across the restart, exactly.
  auto newest = sub.TakeLatest();
  ASSERT_EQ(newest.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*newest).plan_id, 1049u);
  EXPECT_EQ(1u + msg::introspect::OverwriteCount(sub), 100u)
      << "ordinals must be contiguous across the publisher restart (M4-A2)";
  EXPECT_EQ(msg::introspect::TakeCount(sub), 1u);
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

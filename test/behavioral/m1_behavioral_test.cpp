/*
 * m1_behavioral_test.cpp — M1: planner -> control at rate (P0b, in-process).
 *
 * Asserts the M1 acceptance criteria from docs/scenarios.md against the
 * real in-process reach:
 *   A1 — conservation: every published plan is seen at least once OR the
 *        overwrite counter accounts for it; no phantom values.
 *   A2 — tear check under load (run under TSan via XMMESSAGING_SANITIZE).
 *   A3 — staleness/deadline verdicts per take (D2/D3).
 *   A4 — allocation-free publish and take paths once wired (R7).
 *   A5 — the library's own counters reconcile with ground truth (D9).
 *
 * Test conditions stated per family rule: in-process reach, one producer
 * thread and one consumer thread (tests own all threads — R3), payload is
 * the M1 TrajectoryHead-shaped POD (~300 B).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "xmbase/testing/alloc_probe.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

// Scenario-local POD shaped like the real planning vocabulary (M1).
struct TrajectoryHead {
  std::uint64_t plan_id;
  double x[8];
  double y[8];
  double v[8];
  double t_offset[8];
  std::uint32_t checksum;  // M1-A2: written last, must agree with plan_id
};
static_assert(std::is_trivially_copyable_v<TrajectoryHead>);

// Deterministic payload + checksum so a torn value is detectable: every
// field derives from plan_id; checksum is a mix of ALL of them.
std::uint32_t ExpectedChecksum(std::uint64_t id) {
  std::uint64_t h = id * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 32;
  return static_cast<std::uint32_t>(h);
}

void FillPlan(TrajectoryHead& plan, std::uint64_t id) {
  plan.plan_id = id;  // marker field, written first
  for (int i = 0; i < 8; ++i) {
    plan.x[i] = static_cast<double>(id) + i;
    plan.y[i] = static_cast<double>(id) - i;
    plan.v[i] = static_cast<double>(id) * 0.5 + i;
    plan.t_offset[i] = static_cast<double>(i) * 0.1;
  }
  plan.checksum = ExpectedChecksum(id);  // written last
}

// Returns true iff the value is internally consistent (untorn).
bool PlanConsistent(const TrajectoryHead& plan) {
  if (plan.checksum != ExpectedChecksum(plan.plan_id)) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if (plan.x[i] != static_cast<double>(plan.plan_id) + i ||
        plan.y[i] != static_cast<double>(plan.plan_id) - i) {
      return false;
    }
  }
  return true;
}

}  // namespace

// -- A1: conservation — published == unique-seen + overwritten, exactly ------
TEST(M1Behavioral, A1_Conservation) {
  auto domain = msg::Domain::InProcess({.name = "m1_a1"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  constexpr std::uint64_t kPublished = 20000;
  std::atomic<bool> producer_done{false};

  std::thread planner([&] {
    TrajectoryHead plan{};
    for (std::uint64_t id = 1; id <= kPublished; ++id) {
      FillPlan(plan, id);
      EXPECT_EQ(pub.Publish(plan), msg::PublishStatus::kOk);  // M1-A1: latest-
      // only never refuses for capacity
      if ((id & 0x3F) == 0) {
        std::this_thread::yield();  // give the consumer scheduling air
      }
    }
    producer_done.store(true);
  });

  std::uint64_t unique_seen = 0;
  std::uint64_t last_id = 0;
  bool phantom = false;
  bool out_of_order = false;
  const auto consume_once = [&] {
    auto plan = sub.TakeLatest();
    if (plan.freshness() == msg::Freshness::kNone) {
      return;
    }
    const std::uint64_t id = (*plan).plan_id;
    if (id == 0 || id > kPublished || !PlanConsistent(*plan)) {
      phantom = true;  // a value nobody published
    }
    if (id < last_id) {
      out_of_order = true;  // latest-only must be monotonic per publisher
    }
    if (id != last_id) {
      ++unique_seen;
      last_id = id;
    }
  };

  std::thread control([&] {
    // Consume until the producer finished AND the final plan was taken —
    // the LatestMailbox guarantees the newest value is always takeable.
    while (!producer_done.load() || last_id != kPublished) {
      consume_once();
    }
  });

  planner.join();
  control.join();

  EXPECT_FALSE(phantom);
  EXPECT_FALSE(out_of_order);
  // The conservation law, exact (no silent loss, no phantom values):
  EXPECT_EQ(unique_seen + msg::introspect::OverwriteCount(sub), kPublished);
  EXPECT_GE(unique_seen, 1u);
}

// -- A2: never torn, at full rate (M1-A2 requires this test TSan-clean) ------
TEST(M1Behavioral, A2_TearCheckUnderLoad) {
  auto domain = msg::Domain::InProcess({.name = "m1_a2"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});

  std::atomic<bool> running{true};
  std::uint64_t torn = 0;
  std::uint64_t takes = 0;

  std::thread planner([&] {
    TrajectoryHead plan{};
    std::uint64_t id = 0;
    while (running.load(std::memory_order_relaxed)) {
      FillPlan(plan, ++id);
      pub.Publish(plan);  // flat out: maximum overwrite pressure
    }
  });

  std::thread control([&] {
    while (running.load(std::memory_order_relaxed)) {
      auto plan = sub.TakeLatest();
      if (plan.freshness() != msg::Freshness::kNone) {
        ++takes;
        if (!PlanConsistent(*plan)) {
          ++torn;
        }
      }
    }
  });

  std::this_thread::sleep_for(1500ms);
  running.store(false);
  planner.join();
  control.join();

  EXPECT_EQ(torn, 0u) << "torn value observed in " << takes << " takes";
  EXPECT_GT(takes, 1000u) << "consumer starved — retry bias broken?";
}

// -- A3: tri-state freshness against the wiring-time deadline (D2/D3) --------
TEST(M1Behavioral, A3_StalenessDeadline) {
  constexpr auto kDeadline = 200ms;
  auto domain = msg::Domain::InProcess({.name = "m1_a3"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head",
      {.history = msg::History::LatestOnly(), .deadline = kDeadline});

  // Never received: kNone, distinct from kStale (different incidents).
  EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kNone);

  TrajectoryHead plan{};
  FillPlan(plan, 1);
  pub.Publish(plan);

  // Within the deadline: kFresh, and age is a real measurement.
  auto fresh = sub.TakeLatest();
  ASSERT_EQ(fresh.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ(fresh.age_class(), msg::AgeClass::kMeasured);  // D12: in-process
  EXPECT_LT(fresh.age(), kDeadline);

  // Planner paused past the deadline: kStale — the value stays accessible
  // (D2) and its age tells the truth.
  std::this_thread::sleep_for(kDeadline + 150ms);
  auto stale = sub.TakeLatest();
  ASSERT_EQ(stale.freshness(), msg::Freshness::kStale);
  EXPECT_EQ((*stale).plan_id, 1u);  // still accessible
  EXPECT_GT(stale.age(), kDeadline);

  // A new publish recovers to kFresh.
  FillPlan(plan, 2);
  pub.Publish(plan);
  EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kFresh);
}

// -- A4: zero heap allocations on publish and take once wired (R7) -----------
TEST(M1Behavioral, A4_AllocationFreeHotPath) {
  auto domain = msg::Domain::InProcess({.name = "m1_a4"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});

  TrajectoryHead plan{};
  FillPlan(plan, 1);
  pub.Publish(plan);           // warm-up
  (void)sub.TakeLatest();      // warm-up

  std::uint64_t consumed = 0;
  {
    xmotion::testing::AllocProbe probe;  // counts this thread's allocations
    for (std::uint64_t id = 2; id <= 10001; ++id) {
      FillPlan(plan, id);
      pub.Publish(plan);
      auto taken = sub.TakeLatest();
      if (taken.freshness() != msg::Freshness::kNone) {
        consumed += (*taken).plan_id != 0 ? 1 : 0;
      }
      // Loan path is hot-path too (M1 wish-code publishes via Loan):
      auto loan = pub.Loan();
      if (loan.status() == msg::LoanStatus::kOk) {
        FillPlan(*loan, id);
        pub.Publish(std::move(loan));
      }
    }
    EXPECT_EQ(probe.allocations(), 0u)
        << "hot path allocated after wiring (R7 violation)";
  }
  EXPECT_EQ(consumed, 10000u);
}

// -- A5: the library's own counters reconcile with ground truth (D9) ---------
TEST(M1Behavioral, A5_CounterReconciliation) {
  auto domain = msg::Domain::InProcess({.name = "m1_a5"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});

  constexpr std::uint64_t kPublished = 100;
  TrajectoryHead plan{};
  std::uint64_t ground_truth_takes = 0;
  std::uint64_t unique_seen = 0;
  std::uint64_t last_id = 0;

  for (std::uint64_t id = 1; id <= kPublished; ++id) {
    FillPlan(plan, id);
    ASSERT_EQ(pub.Publish(plan), msg::PublishStatus::kOk);
    if ((id % 3) == 0) {  // consumer slower than the producer: overwrites
      auto taken = sub.TakeLatest();
      ASSERT_NE(taken.freshness(), msg::Freshness::kNone);
      ++ground_truth_takes;
      if ((*taken).plan_id != last_id) {
        ++unique_seen;
        last_id = (*taken).plan_id;
      }
    }
  }
  // Final drain so every gap is materialized.
  auto last = sub.TakeLatest();
  ASSERT_NE(last.freshness(), msg::Freshness::kNone);
  ++ground_truth_takes;
  if ((*last).plan_id != last_id) {
    ++unique_seen;
    last_id = (*last).plan_id;
  }
  ASSERT_EQ(last_id, kPublished);

  EXPECT_EQ(msg::introspect::PublishCount(pub), kPublished);
  EXPECT_EQ(msg::introspect::RefusedCount(pub), 0u);  // never on latest-only
  EXPECT_EQ(msg::introspect::TakeCount(sub), ground_truth_takes);
  EXPECT_EQ(unique_seen + msg::introspect::OverwriteCount(sub), kPublished);
}

// ============================================================================
// P1b: the M1 contract across a REAL process boundary (POSIX-shm reach).
// Producer is a forked+exec'd child (shm_test_helper); the consumer side —
// conservation, staleness, counters — is this process. Same acceptance
// criteria, different reach (the M6-A1 spirit applied to M1's cross-process
// leg). TSan note: cross-process shm races are invisible to TSan (see
// shm_test_support.hpp); the ASan leg covers mapping/bounds bugs.
// ============================================================================
#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include "shm_test_support.hpp"

TEST(M1Behavioral, ShmCrossProcess_ConservationAndStaleness) {
  const std::string domain_name = shmtest::UniqueDomainName("m1shm");
  shmtest::SegmentJanitor janitor(domain_name, {"m1.plan.head"});

  // Subscribe FIRST (order independence, M14-A1: the segment is created by
  // whoever arrives first — here the subscriber).
  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      "m1.plan.head",
      {.history = msg::History::LatestOnly(), .deadline = 250ms});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  constexpr std::uint64_t kPublished = 500;
  shmtest::ChildGuard producer(shmtest::SpawnHelper(
      {"publish_stream", domain_name, "m1.plan.head", "1",
       std::to_string(kPublished), "500"}));  // 500 us period
  ASSERT_GT(producer.pid(), 0);

  // Consume at the parent's own cadence while the child streams (D5): count
  // unique values, verify none is torn or phantom (A1/A2 cross-process).
  std::uint64_t unique_seen = 0;
  std::uint64_t last_id = 0;
  bool phantom = false;
  bool out_of_order = false;
  const auto consume_once = [&] {
    auto plan = sub.TakeLatest();
    if (plan.freshness() == msg::Freshness::kNone) {
      return;
    }
    const std::uint64_t id = (*plan).plan_id;
    if (id == 0 || id > kPublished || !ShmPlanConsistent(*plan)) {
      phantom = true;
    }
    if (id < last_id) {
      out_of_order = true;
    }
    if (id != last_id) {
      ++unique_seen;
      last_id = id;
    }
  };
  // Bounded consumption: the child exits after kPublished publishes.
  const auto consume_deadline = xmotion::Now() + 30s;
  while (last_id < kPublished && xmotion::Now() < consume_deadline) {
    consume_once();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  EXPECT_EQ(producer.Reap(), 0) << "producer child failed";
  consume_once();  // final drain: the newest value is the last published

  ASSERT_FALSE(phantom) << "torn or phantom value across the process boundary";
  ASSERT_FALSE(out_of_order) << "latest-only must never go backwards";
  ASSERT_EQ(last_id, kPublished);

  // A1/A5 conservation, exactly, via the SHARED slot counters (D9): every
  // published value was seen once or counted overwritten.
  EXPECT_EQ(unique_seen + msg::introspect::OverwriteCount(sub), kPublished);

  // A3 staleness: the producer is gone; the deadline verdict must raise.
  std::this_thread::sleep_for(300ms);
  auto stale = sub.TakeLatest();
  EXPECT_EQ(stale.freshness(), msg::Freshness::kStale);
  EXPECT_EQ((*stale).plan_id, kPublished);
  EXPECT_GE(stale.age(), 250ms);

  // The graceful exit released the publisher slot: observable (M4-A4 kin).
  EXPECT_EQ(sub.MatchedCount(), 0u);
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

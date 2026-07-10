/*
 * m2_behavioral_test.cpp — M2: mid-run subscriber join (P0b, in-process).
 *
 * Asserts the M2 acceptance criteria from docs/scenarios.md:
 *   A1 — warm start: a late joiner's first take yields the current value
 *        with its ORIGINAL stamp (D6: age never reports ~0 for old data).
 *   A2 — joining and leaving does not perturb the incumbent subscriber.
 *   A3 — N subscribers each independently hold the LatestMailbox contract.
 *
 * Test conditions: in-process reach, single-threaded except where noted
 * (perturbation is a wiring-order property, not a race property).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

struct RobotState {
  double x, y, yaw;
  std::uint64_t seq;
};

}  // namespace

// -- A1: warm start from the current slot value, original stamp --------------
TEST(M2Behavioral, A1_WarmStartWithOriginalStamp) {
  auto domain = msg::Domain::InProcess({.name = "m2_a1"});
  auto pub = domain.Advertise<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  pub.Publish({.x = 1.0, .y = 2.0, .yaw = 0.5, .seq = 1});  // t0
  const auto t0 = xmotion::Now();

  std::this_thread::sleep_for(150ms);

  // Joining mid-run is the SAME Subscribe call as wiring time (D7).
  auto late = domain.Subscribe<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});
  auto first = late.TakeLatest();

  // Immediately kFresh (no deadline declared -> kStale never applies)...
  ASSERT_EQ(first.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*first).seq, 1u);
  // ...and the stamp is the ORIGINAL publish stamp: age reports ~150 ms,
  // never ~0 — a warm start must not make old data look new (R8/D6).
  EXPECT_GE(first.age(), 150ms);
  EXPECT_LE(first.stamp(), t0);
  // Lineage of a first-hop publish survives the warm start too (D14).
  EXPECT_EQ(first.hop_count(), 0u);
  EXPECT_EQ(first.origin_stamp(), first.stamp());

  // A late joiner WITH a deadline judges the warm-start value honestly:
  // the seeded value is older than 50 ms, so its verdict is kStale — the
  // value is still accessible (D2), it just cannot masquerade as fresh.
  auto late_with_deadline = domain.Subscribe<RobotState>(
      "m2.robot.state",
      {.history = msg::History::LatestOnly(), .deadline = 50ms});
  auto judged = late_with_deadline.TakeLatest();
  ASSERT_EQ(judged.freshness(), msg::Freshness::kStale);
  EXPECT_EQ((*judged).seq, 1u);
}

// -- A2: the incumbent is unperturbed by joins and leaves ---------------------
TEST(M2Behavioral, A2_IncumbentUnperturbed) {
  auto domain = msg::Domain::InProcess({.name = "m2_a2"});
  auto pub = domain.Advertise<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});
  auto incumbent = domain.Subscribe<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  constexpr std::uint64_t kBurst = 100;
  std::uint64_t incumbent_unique = 0;
  std::uint64_t last_seq = 0;

  for (std::uint64_t seq = 1; seq <= kBurst; ++seq) {
    // Interleave joins, takes-by-others, and leaves with the burst.
    if (seq % 5 == 0) {
      auto transient = domain.Subscribe<RobotState>(
          "m2.robot.state", {.history = msg::History::LatestOnly()});
      auto taken = transient.TakeLatest();  // another mailbox's take...
      ASSERT_NE(taken.freshness(), msg::Freshness::kNone);
    }  // ...and teardown is scope exit — no unsubscribe ceremony (D7)

    pub.Publish({.x = 0.0, .y = 0.0, .yaw = 0.0, .seq = seq});

    auto seen = incumbent.TakeLatest();
    ASSERT_EQ(seen.freshness(), msg::Freshness::kFresh);
    EXPECT_EQ((*seen).seq, seq) << "incumbent missed or duplicated a value";
    if ((*seen).seq != last_seq) {
      ++incumbent_unique;
      last_seq = (*seen).seq;
    }
  }

  // The incumbent saw EVERY value exactly once: no misses (overwrites) and
  // no duplicates, exactly as if no one had joined or left (M2-A2).
  EXPECT_EQ(incumbent_unique, kBurst);
  EXPECT_EQ(msg::introspect::OverwriteCount(incumbent), 0u);
}

// -- A3: N independent mailboxes, one LatestMailbox contract each ------------
TEST(M2Behavioral, A3_IndependentMailboxes) {
  auto domain = msg::Domain::InProcess({.name = "m2_a3"});
  auto pub = domain.Advertise<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  auto sub_a = domain.Subscribe<RobotState>("m2.robot.state",
                                            {.history = msg::History::LatestOnly()});
  auto sub_b = domain.Subscribe<RobotState>("m2.robot.state",
                                            {.history = msg::History::LatestOnly()});
  auto sub_c = domain.Subscribe<RobotState>("m2.robot.state",
                                            {.history = msg::History::LatestOnly()});

  pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = 1});

  // a and b take value 1; c deliberately does not.
  EXPECT_EQ((*sub_a.TakeLatest()).seq, 1u);
  EXPECT_EQ((*sub_b.TakeLatest()).seq, 1u);

  pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = 2});

  // Every mailbox independently holds the newest value...
  EXPECT_EQ((*sub_a.TakeLatest()).seq, 2u);
  EXPECT_EQ((*sub_b.TakeLatest()).seq, 2u);
  EXPECT_EQ((*sub_c.TakeLatest()).seq, 2u);

  // ...and the overwrite accounting is per-subscriber truth (D7/D8):
  // c never read value 1 before it was overwritten; a and b did.
  EXPECT_EQ(msg::introspect::OverwriteCount(sub_a), 0u);
  EXPECT_EQ(msg::introspect::OverwriteCount(sub_b), 0u);
  EXPECT_EQ(msg::introspect::OverwriteCount(sub_c), 1u);

  // Take counts are independent per mailbox too.
  EXPECT_EQ(msg::introspect::TakeCount(sub_a), 2u);
  EXPECT_EQ(msg::introspect::TakeCount(sub_b), 2u);
  EXPECT_EQ(msg::introspect::TakeCount(sub_c), 1u);

  // Repeated takes of an unchanged slot return the same value (a take is a
  // read, not a pop) and never disturb the neighbors.
  EXPECT_EQ((*sub_a.TakeLatest()).seq, 2u);
  EXPECT_EQ((*sub_b.TakeLatest()).seq, 2u);
}

// -- D6 regression: a joiner is never charged for pre-join values ------------
//
// The overwrite baseline must be the topic's accepted ordinal AT the attach
// point, not a value read before it. A stale (pre-attach) baseline charges
// the joiner's overwrite counter with values that were never delivered to
// its mailbox — including the warm-start seed itself, which D6 classifies
// as pre-join history ("values published before the join are not charged").
// This test is deterministic: with the stale baseline (seed ordinal - 1)
// the first post-join take charges the overwritten SEED as an overwrite.
TEST(M2Behavioral, D6_JoinerNotChargedForPreJoinValues) {
  auto domain = msg::Domain::InProcess({.name = "m2_d6"});
  auto pub = domain.Advertise<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  for (std::uint64_t seq = 1; seq <= 5; ++seq) {
    pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = seq});
  }

  auto joiner = domain.Subscribe<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  // The joiner never reads its warm-start seed (value 5); the next publish
  // overwrites it. Everything up to and including the seed is PRE-JOIN
  // history — none of it may appear on the joiner's overwrite counter.
  pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = 6});
  auto first = joiner.TakeLatest();
  ASSERT_EQ(first.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*first).seq, 6u);
  EXPECT_EQ(msg::introspect::OverwriteCount(joiner), 0u)
      << "pre-join values (including the unread warm-start seed) were "
         "charged to the joiner's overwrite counter (D6 violation)";

  // POST-join overwrites ARE charged, exactly: value 7 was delivered to
  // this mailbox and overwritten unread by value 8.
  pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = 7});
  pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = 8});
  EXPECT_EQ((*joiner.TakeLatest()).seq, 8u);
  EXPECT_EQ(msg::introspect::OverwriteCount(joiner), 1u);
}

// -- D6 regression: mid-stream join races an in-flight publisher --------------
//
// Subscribe() runs under the wiring mutex, but the latest-only Publish hot
// path never takes that mutex — a joiner's accounting baseline therefore
// races in-flight publishes (TOCTOU: baseline read vs. mailbox attach).
// Publishes that land between the two are invisible to the new mailbox
// (correct) and must NOT be charged to the joiner's overwrite counter.
//
// Detection: seq is published 1,2,3,... by a single publisher on a topic
// that never refuses, so payload seq == accepted ordinal exactly. At each
// quiescent point `published` is the topic's accepted ordinal, so a floor
// F read from it before Subscribe() bounds every legitimate charge: a
// correct baseline is >= F, hence after the joiner's FIRST take (ordinal
// o1) its overwrite counter is at most o1 - F - 1. A stale baseline
// charges the seed and every in-flight publish that raced the join, which
// pushes the counter past that bound. The publisher sweeps its burst start
// across the join window (deterministic delay ladder) so, over the join
// cycles, bursts land inside the Subscribe() race window with real
// probability; the assertion itself holds for ALL interleavings of a
// correct implementation (no flake), and any single overcount trips it.
TEST(M2Behavioral, D6_MidStreamJoinAccountingRace) {
  constexpr int kCycles = 200;
  constexpr int kBurst = 2000;

  auto domain = msg::Domain::InProcess({.name = "m2_d6_race"});
  auto pub = domain.Advertise<RobotState>(
      "m2.race.state", {.history = msg::History::LatestOnly()});

  std::atomic<int> go{0};
  std::atomic<int> started{0};
  std::atomic<int> done{0};
  std::atomic<std::uint64_t> published{0};

  std::thread publisher([&] {
    std::uint64_t seq = 0;
    int cycle = 0;
    for (;;) {
      const int g = go.load(std::memory_order_acquire);
      if (g < 0) {
        break;
      }
      if (g == cycle) {
        std::this_thread::yield();
        continue;
      }
      cycle = g;
      // Delay ladder: walk the burst start across the Subscribe() window
      // (0..20us in 500ns steps) so cycles land before, inside, and after
      // the join even as build/instrumentation timing shifts.
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds(500) * (cycle % 40);
      while (std::chrono::steady_clock::now() < until) {
      }
      for (int i = 0; i < kBurst; ++i) {
        pub.Publish({.x = 0, .y = 0, .yaw = 0, .seq = ++seq});
        if (i == 0) {
          started.store(cycle, std::memory_order_release);
        }
      }
      published.store(seq, std::memory_order_release);
      done.store(cycle, std::memory_order_release);
    }
  });

  for (int c = 1; c <= kCycles; ++c) {
    // Quiescent floor: the publisher is idle (done == c-1), so `published`
    // IS the accepted ordinal right now; the join point can only be later.
    const std::uint64_t floor = published.load(std::memory_order_acquire);

    go.store(c, std::memory_order_release);        // burst races the join
    auto joiner = domain.Subscribe<RobotState>(
        "m2.race.state", {.history = msg::History::LatestOnly()});

    // Hold the first take until the burst is in flight. This maximizes the
    // exposure of the joiner's accounting baseline: if the seed were taken
    // first the counter would start from the seed regardless of where the
    // baseline was placed, masking a stale one.
    while (started.load(std::memory_order_acquire) != c) {
      std::this_thread::yield();
    }
    msg::Sample<RobotState> first;
    do {
      first = joiner.TakeLatest();
    } while (first.freshness() == msg::Freshness::kNone);
    const std::uint64_t o1 = (*first).seq;
    const std::uint64_t w1 = msg::introspect::OverwriteCount(joiner);

    // THE regression assertion. Values <= floor predate the join; a
    // correct baseline sits at or above it, so the first take can charge
    // at most the post-floor, pre-o1 values. A stale baseline also
    // charges the seed + the publishes that raced Subscribe() itself.
    const std::uint64_t bound = o1 > floor ? o1 - floor - 1 : 0;
    EXPECT_LE(w1, bound)
        << "cycle " << c << ": joiner overwrite counter charged values "
        << "never delivered to it (stale pre-attach baseline); first-seen "
        << "ordinal " << o1 << ", pre-join floor " << floor;
    if (w1 > bound) {
      break;  // failure recorded; exit so the publisher joins cleanly
    }

    // Drain the burst, then reconcile the post-join stream exactly: from
    // the second distinct value onward every publisher ordinal must be
    // accounted as seen or overwritten — no loss, no phantom (M1-A5/D9).
    std::uint64_t last_seen = o1;
    std::uint64_t o2 = 0;
    std::uint64_t w2 = 0;
    std::uint64_t distinct_after_o2 = 0;
    for (;;) {
      auto taken = joiner.TakeLatest();
      const std::uint64_t v = (*taken).seq;
      if (v != last_seen) {
        last_seen = v;
        if (o2 == 0) {
          o2 = v;
          w2 = msg::introspect::OverwriteCount(joiner);
        } else {
          ++distinct_after_o2;
        }
      }
      if (done.load(std::memory_order_acquire) == c &&
          last_seen == published.load(std::memory_order_acquire)) {
        break;
      }
    }
    if (o2 != 0) {
      const std::uint64_t final_seq = published.load(std::memory_order_acquire);
      EXPECT_EQ(msg::introspect::OverwriteCount(joiner) - w2 +
                    distinct_after_o2,
                final_seq - o2)
          << "cycle " << c << ": post-join reconciliation broke";
    }
  }

  go.store(-1, std::memory_order_release);
  publisher.join();
}

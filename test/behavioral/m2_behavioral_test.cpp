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

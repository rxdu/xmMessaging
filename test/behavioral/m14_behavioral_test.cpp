/*
 * m14_behavioral_test.cpp — M14: stack cold start (P0b, in-process).
 *
 * Asserts the M14 acceptance criteria from docs/scenarios.md:
 *   A1 — wiring order permutations converge to the same wired state.
 *   A2 — WaitUntilMatched: success exactly when all endpoints have peers;
 *        a DISTINCT timeout status when the deadline passes (bounded).
 *   A3 — exclusive ownership refused with a distinct status; declared
 *        kShared resolves latest-only last-writer-wins by publish stamp.
 *   A4 — isolation keys: two domains share NOTHING.
 *   A5 — MatchedCount() tracks peer join/leave through restart.
 *
 * Plus two D18/R6 wiring-status checks that belong to the cold-start story
 * (type-mismatch refusal; unsupported-reach factories) and the wire-contract
 * §4.1/§5 FNV-1a reference vectors for the schema-hash seam.
 *
 * Test conditions: in-process reach; threads only where the criterion is
 * about concurrency (A2 wait, A3 interleaving).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "xmmsg/detail/schema_hash.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

struct StateEstimate {
  double x, y, yaw, vx, vy;
};

struct Setpoint {
  std::uint64_t writer_id;
  std::uint64_t seq;
  std::uint64_t checksum;  // tear witness for the shared-writer interleaving
};

constexpr std::uint64_t SetpointChecksum(std::uint64_t writer,
                                         std::uint64_t seq) {
  return (writer * 0x9E3779B97F4A7C15ULL) ^ seq;
}

}  // namespace

// -- A1: every wiring order converges to the same wired state ----------------
TEST(M14Behavioral, A1_WiringOrderPermutations) {
  // Permutation 1: Subscribe before Advertise (the launcher race case).
  {
    auto domain = msg::Domain::InProcess({.name = "m14_a1_sub_first"});
    auto sub = domain.Subscribe<StateEstimate>("m14.estimator.state", {});
    EXPECT_EQ(sub.MatchedCount(), 0u);
    // Silent and normal, not an error: freshness is kNone until matched.
    EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kNone);

    auto pub = domain.Advertise<StateEstimate>(
        "m14.estimator.state", {.history = msg::History::LatestOnly()});
    ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
    EXPECT_EQ(sub.MatchedCount(), 1u);
    EXPECT_EQ(pub.MatchedCount(), 1u);

    pub.Publish({.x = 1, .y = 2, .yaw = 3, .vx = 0, .vy = 0});
    EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kFresh);
  }
  // Permutation 2: Advertise before Subscribe — indistinguishable after
  // matching (the late subscriber even warm-starts, D6).
  {
    auto domain = msg::Domain::InProcess({.name = "m14_a1_pub_first"});
    auto pub = domain.Advertise<StateEstimate>(
        "m14.estimator.state", {.history = msg::History::LatestOnly()});
    pub.Publish({.x = 1, .y = 2, .yaw = 3, .vx = 0, .vy = 0});

    auto sub = domain.Subscribe<StateEstimate>("m14.estimator.state", {});
    EXPECT_EQ(sub.MatchedCount(), 1u);
    EXPECT_EQ(pub.MatchedCount(), 1u);
    EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kFresh);
  }
}

// -- A2: the one bounded readiness barrier (D16) ------------------------------
TEST(M14Behavioral, A2_WaitUntilMatched) {
  auto domain = msg::Domain::InProcess({.name = "m14_a2"});
  auto est_sub = domain.Subscribe<StateEstimate>("m14.estimator.state", {});

  // Success case: the estimator comes up ~100 ms into the wait; the barrier
  // returns kMatched well inside the deadline.
  std::atomic<bool> release{false};
  std::thread estimator([&] {
    std::this_thread::sleep_for(100ms);
    auto pub = domain.Advertise<StateEstimate>(
        "m14.estimator.state", {.history = msg::History::LatestOnly()});
    ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
    while (!release.load()) {
      std::this_thread::sleep_for(1ms);  // keep the peer alive
    }
  });

  const auto wait_start = xmotion::Now();
  const msg::WaitStatus matched =
      domain.WaitUntilMatched({&est_sub}, std::chrono::seconds(10));
  const auto waited = xmotion::Now() - wait_start;
  EXPECT_EQ(matched, msg::WaitStatus::kMatched);
  EXPECT_LT(waited, std::chrono::seconds(10));  // returned on the event,
  EXPECT_EQ(est_sub.MatchedCount(), 1u);        // not the deadline
  release.store(true);
  estimator.join();

  // Timeout case: an endpoint whose peer never appears yields the DISTINCT
  // status, bounded by the deadline — never a hang, never fake success.
  auto orphan = domain.Subscribe<StateEstimate>("m14.never.advertised", {});
  const auto timeout_start = xmotion::Now();
  const msg::WaitStatus expired =
      domain.WaitUntilMatched({&est_sub, &orphan}, 150ms);
  const auto timeout_waited = xmotion::Now() - timeout_start;
  EXPECT_EQ(expired, msg::WaitStatus::kDeadlineExpired);
  EXPECT_GE(timeout_waited, 150ms);
  EXPECT_LT(timeout_waited, std::chrono::seconds(5));
  // WHO is missing is queryable per endpoint (the launcher reports it):
  EXPECT_EQ(orphan.MatchedCount(), 0u);
}

// -- A3: ownership — refusal by default, last-writer-wins by declaration -----
TEST(M14Behavioral, A3_OwnershipRefusalAndSharedLastWriterWins) {
  auto domain = msg::Domain::InProcess({.name = "m14_a3"});

  // Exclusive default: the duplicate estimator is REFUSED, first untouched.
  auto est_pub = domain.Advertise<StateEstimate>(
      "m14.estimator.state", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(est_pub.status(), msg::AdvertiseStatus::kOk);
  auto dup = domain.Advertise<StateEstimate>("m14.estimator.state", {});
  EXPECT_EQ(dup.status(), msg::AdvertiseStatus::kOwnershipRefused);

  auto est_sub = domain.Subscribe<StateEstimate>("m14.estimator.state", {});
  est_pub.Publish({.x = 7, .y = 0, .yaw = 0, .vx = 0, .vy = 0});
  auto seen = est_sub.TakeLatest();
  ASSERT_EQ(seen.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*seen).x, 7.0);  // the incumbent kept working

  // Shared is a declaration made by BOTH: exclusive + shared is refused too.
  auto half_shared = domain.Advertise<StateEstimate>(
      "m14.estimator.state", {.ownership = msg::Ownership::kShared});
  EXPECT_EQ(half_shared.status(), msg::AdvertiseStatus::kOwnershipRefused);

  // The deliberate case: planner + recovery planner, both declared kShared.
  auto primary = domain.Advertise<Setpoint>(
      "m14.control.setpoint", {.ownership = msg::Ownership::kShared});
  auto recovery = domain.Advertise<Setpoint>(
      "m14.control.setpoint", {.ownership = msg::Ownership::kShared});
  ASSERT_EQ(primary.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(recovery.status(), msg::AdvertiseStatus::kOk);
  auto sp_sub = domain.Subscribe<Setpoint>("m14.control.setpoint", {});

  // Sequential interleaving: every take resolves to the newest stamp.
  primary.Publish({.writer_id = 1, .seq = 1, .checksum = SetpointChecksum(1, 1)});
  recovery.Publish({.writer_id = 2, .seq = 1, .checksum = SetpointChecksum(2, 1)});
  EXPECT_EQ((*sp_sub.TakeLatest()).writer_id, 2u);
  primary.Publish({.writer_id = 1, .seq = 2, .checksum = SetpointChecksum(1, 2)});
  EXPECT_EQ((*sp_sub.TakeLatest()).writer_id, 1u);

  // Concurrent interleaving: deterministic under any schedule — the taken
  // value is always internally consistent (never torn, never interleaved
  // across writers) and carries a monotonically resolvable stamp.
  std::atomic<bool> go{false};
  auto writer = [&go](msg::Publisher<Setpoint>& pub, std::uint64_t id) {
    while (!go.load()) {
    }
    for (std::uint64_t seq = 1; seq <= 2000; ++seq) {
      pub.Publish({.writer_id = id, .seq = seq,
                   .checksum = SetpointChecksum(id, seq)});
    }
  };
  std::thread w1(writer, std::ref(primary), 1);
  std::thread w2(writer, std::ref(recovery), 2);
  go.store(true);

  msg::Timestamp last_stamp{};
  for (int i = 0; i < 5000; ++i) {
    auto taken = sp_sub.TakeLatest();
    ASSERT_NE(taken.freshness(), msg::Freshness::kNone);
    EXPECT_EQ((*taken).checksum,
              SetpointChecksum((*taken).writer_id, (*taken).seq));
    EXPECT_GE(taken.stamp(), last_stamp);  // resolution is by publish stamp,
    last_stamp = taken.stamp();            // monotone at the subscriber
  }
  w1.join();
  w2.join();

  // Post-quiescence: the last writer wins, deterministically.
  primary.Publish({.writer_id = 9, .seq = 9, .checksum = SetpointChecksum(9, 9)});
  EXPECT_EQ((*sp_sub.TakeLatest()).writer_id, 9u);
}

// -- A4: isolation keys — two stacks on one host share nothing ---------------
TEST(M14Behavioral, A4_IsolationKeys) {
  auto stack_a = msg::Domain::InProcess({.name = "m14_a4_stack_a"});
  auto stack_b = msg::Domain::InProcess({.name = "m14_a4_stack_b"});

  auto a_pub = stack_a.Advertise<StateEstimate>(
      "m14.estimator.state", {.history = msg::History::LatestOnly()});
  auto b_sub = stack_b.Subscribe<StateEstimate>("m14.estimator.state", {});

  a_pub.Publish({.x = 1, .y = 1, .yaw = 1, .vx = 1, .vy = 1});

  // Same topic string, different domain key: no visibility, ever (negative).
  EXPECT_EQ(b_sub.MatchedCount(), 0u);
  EXPECT_EQ(b_sub.TakeLatest().freshness(), msg::Freshness::kNone);

  // Positive: stack_b's own wiring works normally.
  auto b_pub = stack_b.Advertise<StateEstimate>(
      "m14.estimator.state", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(b_pub.status(), msg::AdvertiseStatus::kOk);  // no cross-domain
  b_pub.Publish({.x = 2, .y = 2, .yaw = 2, .vx = 2, .vy = 2});  // ownership
  auto b_seen = b_sub.TakeLatest();
  ASSERT_EQ(b_seen.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*b_seen).x, 2.0);

  // Two Domain handles with the SAME key are one domain (D17: the key is
  // the identity) — the estimator's state is visible to its twin handle.
  auto stack_a_again = msg::Domain::InProcess({.name = "m14_a4_stack_a"});
  auto a_sub = stack_a_again.Subscribe<StateEstimate>("m14.estimator.state", {});
  EXPECT_EQ(a_sub.MatchedCount(), 1u);
  EXPECT_EQ(a_sub.TakeLatest().freshness(), msg::Freshness::kFresh);
}

// -- A5: MatchedCount tracks join/leave through restart ----------------------
TEST(M14Behavioral, A5_MatchedCountTracking) {
  auto domain = msg::Domain::InProcess({.name = "m14_a5"});
  auto est_sub = domain.Subscribe<StateEstimate>("m14.estimator.state", {});
  EXPECT_EQ(est_sub.MatchedCount(), 0u);

  {
    auto est_pub = domain.Advertise<StateEstimate>(
        "m14.estimator.state", {.history = msg::History::LatestOnly()});
    ASSERT_EQ(est_pub.status(), msg::AdvertiseStatus::kOk);
    EXPECT_EQ(est_sub.MatchedCount(), 1u);
  }  // estimator "dies": scope exit (D7)

  EXPECT_EQ(est_sub.MatchedCount(), 0u);

  // Restart: re-advertising the same exclusive topic succeeds (ownership
  // was released with the endpoint), and the incumbent subscription serves
  // fresh values with NO re-wiring code.
  auto restarted = domain.Advertise<StateEstimate>(
      "m14.estimator.state", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(restarted.status(), msg::AdvertiseStatus::kOk);
  EXPECT_EQ(est_sub.MatchedCount(), 1u);
  restarted.Publish({.x = 5, .y = 0, .yaw = 0, .vx = 0, .vy = 0});
  auto seen = est_sub.TakeLatest();
  ASSERT_EQ(seen.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*seen).x, 5.0);
}

// -- D18/R6: wiring statuses that belong to the cold-start story -------------
TEST(M14Behavioral, WiringStatus_TypeMismatchRefused) {
  auto domain = msg::Domain::InProcess({.name = "m14_types"});
  auto pub = domain.Advertise<StateEstimate>("m14.typed.topic", {});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);

  // A different payload type on the established topic: refused with the
  // DISTINCT status on both verbs (R6) — never a reinterpreted byte.
  auto bad_sub = domain.Subscribe<Setpoint>("m14.typed.topic", {});
  EXPECT_EQ(bad_sub.status(), msg::SubscribeStatus::kTypeMismatch);
  EXPECT_EQ(bad_sub.MatchedCount(), 0u);
  EXPECT_EQ(bad_sub.TakeLatest().freshness(), msg::Freshness::kNone);

  auto bad_pub = domain.Advertise<Setpoint>("m14.typed.topic", {});
  EXPECT_EQ(bad_pub.status(), msg::AdvertiseStatus::kTypeMismatch);

  // The matching type still wires fine (the refusal is local).
  auto good_sub = domain.Subscribe<StateEstimate>("m14.typed.topic", {});
  EXPECT_EQ(good_sub.status(), msg::SubscribeStatus::kOk);
}

TEST(M14Behavioral, WiringStatus_UnsupportedReachIsExplicit) {
  // M8-A2: a backend not compiled into this build never silently falls
  // back to in-process — the handles say so, per endpoint.
  auto domain = msg::Domain::Iceoryx2({.service_name = "m14"});
  EXPECT_EQ(domain.reach_name(), "iceoryx2");
  EXPECT_FALSE(domain.Supports(msg::Contract::kLatestOnly));

  auto pub = domain.Advertise<StateEstimate>("m14.unsupported", {});
  auto sub = domain.Subscribe<StateEstimate>("m14.unsupported", {});
  EXPECT_EQ(pub.status(), msg::AdvertiseStatus::kUnsupportedReach);
  EXPECT_EQ(sub.status(), msg::SubscribeStatus::kUnsupportedReach);
  EXPECT_EQ(domain.WaitUntilMatched({&pub, &sub}, 10ms),
            msg::WaitStatus::kDeadlineExpired);

  // The in-process reach — the reference semantics — supports everything.
  auto inproc = msg::Domain::InProcess({.name = "m14_reach"});
  EXPECT_EQ(inproc.reach_name(), "in-process");
  EXPECT_TRUE(inproc.Supports(msg::Contract::kLatestOnly));
  EXPECT_TRUE(inproc.Supports(msg::Contract::kLateJoinWarmStart));
}

// -- Wire-contract §4.1/§5: the FNV-1a seam reproduces the reference vectors -
TEST(M14Behavioral, SchemaHashFnvReferenceVectors) {
  using xmotion::messaging::detail::Fnv1a64;
  EXPECT_EQ(Fnv1a64(""), 0xCBF29CE484222325ULL);
  EXPECT_EQ(Fnv1a64("a"), 0xAF63DC4C8601EC8CULL);
  EXPECT_EQ(Fnv1a64("foobar"), 0x85944171F73967E8ULL);
  // V1 canonical description from wire-contract §5 (the P1 target form —
  // proves the hash function is already the normative one; only the
  // description generator is interim at P0b).
  EXPECT_EQ(Fnv1a64("size:24\nx:f64:0:8\ny:f64:8:8\ntheta:f64:16:8\n"),
            0xE0978597FA5660D4ULL);
}

/*
 * m3_behavioral_test.cpp — M3: slow consumer, back-pressure, flood (P0b).
 *
 * Asserts the M3 acceptance criteria from docs/scenarios.md:
 *   A1 — reliable: Publish returns kWouldBlock once the queue is full,
 *        never blocks, never drops silently; published == delivered +
 *        explicitly-refused, EXACTLY, and delivery is in order.
 *   A2 — best-effort: Publish always kOk; delivered + counted drops ==
 *        published, EXACTLY (drops are per-subscriber facts, D8).
 *   A4 — the counters are visible programmatically with zero scenario-side
 *        plumbing (D9 introspect; the same atomics feed the messaging.*
 *        instruments).
 *
 * (A3 flood-isolation is a latency-distribution claim — it lands with the
 * M9 benchmark tier; a functional neighbor-health check is included here.)
 *
 * Test conditions: in-process reach, single-threaded — back-pressure is a
 * capacity property, not a race property; M1-A2 covers the concurrency leg.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;

namespace {

struct Sample64 {
  std::uint64_t seq;
  std::uint8_t pad[56];
};

}  // namespace

// -- A1: reliable — explicit refusal, exact conservation, in order -----------
TEST(M3Behavioral, A1_ReliableConservationExact) {
  auto domain = msg::Domain::InProcess({.name = "m3_a1"});
  auto pub = domain.Advertise<Sample64>(
      "m3.reliable.stream", {.history = msg::History::Queue(8),
                             .reliability = msg::Reliability::kReliable});
  auto sub = domain.Subscribe<Sample64>("m3.reliable.stream",
                                        {.history = msg::History::Queue(8)});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // Flood with the consumer stalled (it simply never takes).
  constexpr std::uint64_t kFlood = 1000;
  std::uint64_t accepted = 0;
  std::uint64_t refused = 0;
  for (std::uint64_t i = 0; i < kFlood; ++i) {
    switch (pub.Publish({.seq = i, .pad = {}})) {
      case msg::PublishStatus::kOk:
        ++accepted;
        break;
      case msg::PublishStatus::kWouldBlock:  // full RIGHT NOW, nothing
        ++refused;                           // enqueued — caller policy next
        break;
    }
  }

  // Exact conservation at the publish site: the queue held exactly its
  // declared depth, everything else was refused VISIBLY.
  EXPECT_EQ(accepted, 8u);
  EXPECT_EQ(refused, kFlood - 8u);
  EXPECT_EQ(accepted + refused, kFlood);

  // Everything accepted arrives, in order, exactly once.
  std::uint64_t drained = 0;
  for (;;) {
    auto sample = sub.TryTake();
    if (sample.freshness() == msg::Freshness::kNone) {
      break;
    }
    EXPECT_EQ((*sample).seq, drained);  // in publish order, no gaps
    ++drained;
  }
  EXPECT_EQ(drained, accepted);
  EXPECT_EQ(msg::introspect::DropCount(sub), 0u);  // reliable never drops

  // After draining, capacity is back — refusal is a NOW fact, not a latch.
  EXPECT_EQ(pub.Publish({.seq = 9999, .pad = {}}), msg::PublishStatus::kOk);
  EXPECT_EQ((*sub.TryTake()).seq, 9999u);
}

// -- A2: best-effort — always kOk, drops counted per subscriber, exact -------
TEST(M3Behavioral, A2_BestEffortConservationExact) {
  auto domain = msg::Domain::InProcess({.name = "m3_a2"});
  auto pub = domain.Advertise<Sample64>(
      "m3.besteffort.stream", {.history = msg::History::Queue(8),
                               .reliability = msg::Reliability::kBestEffort});
  auto sub = domain.Subscribe<Sample64>("m3.besteffort.stream",
                                        {.history = msg::History::Queue(8)});

  constexpr std::uint64_t kFlood = 1000;
  for (std::uint64_t i = 0; i < kFlood; ++i) {
    // Best-effort Publish always returns kOk: the transport accepted it;
    // whether every subscriber kept it is each subscriber's queue policy
    // and shows up in THAT subscriber's drop counter (D8).
    EXPECT_EQ(pub.Publish({.seq = i, .pad = {}}), msg::PublishStatus::kOk);
  }

  // Drain: the ring kept the OLDEST 8 (overflow drops the INCOMING value —
  // drop-newest — so what was already accepted is never displaced).
  std::uint64_t delivered = 0;
  for (;;) {
    auto sample = sub.TryTake();
    if (sample.freshness() == msg::Freshness::kNone) {
      break;
    }
    EXPECT_EQ((*sample).seq, delivered);  // seq 0..7, in order
    ++delivered;
  }

  // The Aeron lesson, exact: delivered + counted-drops == published.
  EXPECT_EQ(delivered, 8u);
  EXPECT_EQ(delivered + msg::introspect::DropCount(sub), kFlood);

  // The flood taxed nobody else: a healthy latest-only neighbor sharing
  // the domain still delivers fresh values (functional slice of A3).
  auto pub_c = domain.Advertise<Sample64>("m3.control.heartbeat",
                                          {.history = msg::History::LatestOnly()});
  auto sub_c = domain.Subscribe<Sample64>("m3.control.heartbeat",
                                          {.history = msg::History::LatestOnly()});
  EXPECT_EQ(pub_c.Publish({.seq = 42, .pad = {}}), msg::PublishStatus::kOk);
  auto heartbeat = sub_c.TakeLatest();
  ASSERT_EQ(heartbeat.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*heartbeat).seq, 42u);
}

// -- A4: counters visible programmatically, zero scenario plumbing (D9) ------
TEST(M3Behavioral, A4_CountersVisible) {
  auto domain = msg::Domain::InProcess({.name = "m3_a4"});
  auto pub_r = domain.Advertise<Sample64>(
      "m3.reliable.stream", {.history = msg::History::Queue(4),
                             .reliability = msg::Reliability::kReliable});
  auto sub_r = domain.Subscribe<Sample64>("m3.reliable.stream",
                                          {.history = msg::History::Queue(4)});
  auto pub_b = domain.Advertise<Sample64>(
      "m3.besteffort.stream", {.history = msg::History::Queue(4),
                               .reliability = msg::Reliability::kBestEffort});
  auto sub_b = domain.Subscribe<Sample64>("m3.besteffort.stream",
                                          {.history = msg::History::Queue(4)});

  constexpr std::uint64_t kFlood = 100;
  std::uint64_t accepted_r = 0;
  for (std::uint64_t i = 0; i < kFlood; ++i) {
    if (pub_r.Publish({.seq = i, .pad = {}}) == msg::PublishStatus::kOk) {
      ++accepted_r;
    }
    pub_b.Publish({.seq = i, .pad = {}});
  }

  // Publisher-side truth (the same atomics feed messaging.pub.*):
  EXPECT_EQ(msg::introspect::PublishCount(pub_r), accepted_r);
  EXPECT_EQ(msg::introspect::RefusedCount(pub_r), kFlood - accepted_r);
  EXPECT_EQ(msg::introspect::PublishCount(pub_b), kFlood);
  EXPECT_EQ(msg::introspect::RefusedCount(pub_b), 0u);

  // Subscriber-side truth (messaging.sub.*):
  EXPECT_EQ(msg::introspect::DropCount(sub_r), 0u);
  EXPECT_EQ(msg::introspect::DropCount(sub_b), kFlood - 4u);

  // Drain both and reconcile take counts.
  std::uint64_t taken_r = 0;
  while (sub_r.TryTake().freshness() != msg::Freshness::kNone) {
    ++taken_r;
  }
  std::uint64_t taken_b = 0;
  while (sub_b.TryTake().freshness() != msg::Freshness::kNone) {
    ++taken_b;
  }
  EXPECT_EQ(taken_r, accepted_r);
  EXPECT_EQ(taken_b, 4u);
  EXPECT_EQ(msg::introspect::TakeCount(sub_r), taken_r);
  EXPECT_EQ(msg::introspect::TakeCount(sub_b), taken_b);
  EXPECT_EQ(taken_b + msg::introspect::DropCount(sub_b), kFlood);
}

// ============================================================================
// P1b: M3's best-effort accounting across a REAL process boundary (POSIX-shm
// reach): the flooding publisher counts drops into the subscriber's SHARED
// slot counter, and delivered + counted-drops == published, exactly (M3-A2)
// — reconciled from the subscriber process with zero cooperation from the
// (already exited) publisher. Reliable back-pressure (M3-A1) is a declared
// divergence on this reach: Supports(kReliableQueue) == false and the
// reliable Advertise is refused at wiring — asserted here (M6-A6 pattern).
// ============================================================================
#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include "shm_test_support.hpp"

TEST(M3Behavioral, ShmCrossProcess_BestEffortConservation) {
  const std::string domain_name = shmtest::UniqueDomainName("m3shm");
  shmtest::SegmentJanitor janitor(domain_name, {"m3.flood.best_effort"});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  auto sub = domain.Subscribe<ShmTestPlan>(
      "m3.flood.best_effort", {.history = msg::History::Queue(8)});
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // Stalled consumer: the parent takes NOTHING while the child floods.
  constexpr std::uint64_t kFlood = 1000;
  shmtest::ChildGuard flooder(shmtest::SpawnHelper(
      {"flood_queue", domain_name, "m3.flood.best_effort",
       std::to_string(kFlood)}));
  ASSERT_GT(flooder.pid(), 0);
  ASSERT_EQ(flooder.Reap(), 0) << "flood child failed";

  // Drain after the fact: the ring holds exactly its depth; everything else
  // was dropped-and-counted by the publisher into the shared slot counter.
  std::uint64_t taken = 0;
  bool corrupt = false;
  for (;;) {
    auto sample = sub.TryTake();
    if (sample.freshness() == msg::Freshness::kNone) {
      break;
    }
    if (!ShmPlanConsistent(*sample)) {
      corrupt = true;
    }
    ++taken;
  }
  EXPECT_FALSE(corrupt);
  EXPECT_EQ(taken, 8u) << "an untouched queue<8> holds exactly its depth";
  // M3-A2, cross-process and exact: delivered + counted-drops == published.
  EXPECT_EQ(taken + msg::introspect::DropCount(sub), kFlood);
  EXPECT_EQ(msg::introspect::TakeCount(sub), taken);
}

TEST(M3Behavioral, ShmReliableQueueIsADeclaredDivergence) {
  const std::string domain_name = shmtest::UniqueDomainName("m3rel");
  shmtest::SegmentJanitor janitor(domain_name, {"m3.flood.reliable"});

  auto domain = msg::Domain::PosixShm({.name = domain_name});
  // M6-A6: the divergence is wiring-time knowledge...
  EXPECT_FALSE(domain.Supports(msg::Contract::kReliableQueue));
  // ...and the wiring site agrees with the matrix: refused, never emulated.
  auto pub = domain.Advertise<ShmTestPlan>(
      "m3.flood.reliable", {.history = msg::History::Queue(8),
                            .reliability = msg::Reliability::kReliable});
  EXPECT_EQ(pub.status(), msg::AdvertiseStatus::kUnsupportedReach);
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

/*
 * m6_behavioral_test.cpp — M6: reach transparency (P0b, in-process leg).
 *
 * The library's central promise, tested literally: the assertion bodies
 * below are written ONCE and are byte-identical for every reach (M6-A1) —
 * every per-reach fact lives in the ReachCase parameter, never in a test
 * body. P1 (iceoryx2), P1b (POSIX shm), and P2 (Zenoh) add legs by
 * appending a ReachCase to the INSTANTIATE list; the bodies stay untouched.
 *
 * Asserted here (in-process instantiation):
 *   A1 — fixture-only variance (enforced structurally: TEST_P bodies read
 *        nothing reach-specific except GetParam()).
 *   A2 — the LatestMailbox contract holds (distilled M1 A1-A3; the full-rate
 *        M1 body runs in m1_behavioral_test).
 *   A5 — age_class() matches the reach's clock story (D12/R8): in-process
 *        is kMeasured.
 *   A6 — Supports() answers match the documented per-reach support matrix
 *        (design.md): the in-process reach natively honors every portable
 *        contract.
 *
 * Test conditions stated per family rule: in-process reach, deterministic
 * single-threaded bodies (the racing variants are M1's job), TrajectoryHead
 * -shaped POD payload.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#if defined(XMMESSAGING_HAS_POSIX_SHM)
#include <unistd.h>  // getpid — per-run shm fixture isolation (D17)
#endif

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

struct TrajectoryHead {
  std::uint64_t plan_id;
  double x[8];
  double y[8];
};

// -- The reach fixture: the ONLY thing allowed to vary (M6-A1) ---------------
struct ReachCase {
  const char* name;                  // test-suffix + diagnostics
  msg::Domain (*make)();             // the wiring-time factory (D12)
  msg::AgeClass expected_age_class;  // R8/D12 clock story of this reach
  // Expected Supports() answer per Contract value (M6-A6), indexed by the
  // Contract enumerator. Must transcribe the documented design.md matrix.
  bool supports[8];
};

msg::Domain MakeInProcess() {
  return msg::Domain::InProcess({.name = "m6_fixture"});
}

// In-process: the reference semantics — every portable contract is honored
// natively, and one process means one clock (kMeasured).
constexpr ReachCase kInProcessCase{
    "InProcess",
    &MakeInProcess,
    msg::AgeClass::kMeasured,
    {true, true, true, true, true, true, true, true},
};

constexpr msg::Contract kAllContracts[8] = {
    msg::Contract::kLatestOnly,        msg::Contract::kBoundedQueue,
    msg::Contract::kReliableQueue,     msg::Contract::kZeroCopyLoan,
    msg::Contract::kLateJoinWarmStart, msg::Contract::kRequestResponse,
    msg::Contract::kDeadline,          msg::Contract::kSharedOwnership,
};

#if defined(XMMESSAGING_HAS_POSIX_SHM)
// P1b: the POSIX-shm reach joins the fixture — a new ReachCase and nothing
// else (M6-A1: the TEST_P bodies below are untouched). Both endpoints live
// in this process, attached through the shared segment; the fork-based
// cross-process legs are m1/m2/m3/m4's job.
std::string PosixShmFixtureName() {
  // Per-run domain name: segments never collide across runs (D17).
  static const std::string kName =
      "m6fix_" + std::to_string(static_cast<unsigned long>(::getpid()));
  return kName;
}

msg::Domain MakePosixShm() {
  return msg::Domain::PosixShm({.name = PosixShmFixtureName()});
}

// The honestly-partial P1b support matrix (design.md; posix_shm.hpp):
// latest-only, bounded queue, warm start, deadline — yes; reliable queue,
// zero-copy loan, RPC, shared ownership — declared divergences.
constexpr ReachCase kPosixShmCase{
    "PosixShm",
    &MakePosixShm,
    msg::AgeClass::kMeasured,  // same host, one CLOCK_MONOTONIC (R8)
    {true, true, false, false, true, false, true, false},
};

// The library's documented policy is never-unlink (shm_segment.hpp); the
// fixture cleans up its segments when the test program ends.
class M6ShmCleanup : public ::testing::Environment {
 public:
  void TearDown() override {
    const std::string key =
        msg::detail::DeriveIsolationKey(PosixShmFixtureName());
    msg::detail::UnlinkSegment(msg::detail::ShmSegmentName(key, "m6.plan.head"));
    msg::detail::UnlinkSegment(msg::detail::ShmSegmentName(key, "m6.age.class"));
  }
};
const auto* const kM6ShmCleanup =
    ::testing::AddGlobalTestEnvironment(new M6ShmCleanup());
#endif  // XMMESSAGING_HAS_POSIX_SHM

class M6ReachTransparency : public ::testing::TestWithParam<ReachCase> {};

}  // namespace

// -- A2: the LatestMailbox contract, reach-blind ------------------------------
TEST_P(M6ReachTransparency, LatestMailboxContract) {
  auto domain = GetParam().make();

  auto pub = domain.Advertise<TrajectoryHead>(
      "m6.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m6.plan.head", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // Never received is kNone — a freshness state, not an error (D1/D2).
  EXPECT_EQ(sub.TakeLatest().freshness(), msg::Freshness::kNone);

  // Newest-or-nothing with exact overwrite accounting (guarantees 1 & 3).
  constexpr std::uint64_t kPublished = 10;
  for (std::uint64_t id = 1; id <= kPublished; ++id) {
    TrajectoryHead plan{};
    plan.plan_id = id;
    EXPECT_EQ(pub.Publish(plan), msg::PublishStatus::kOk);  // never refuses
  }
  auto newest = sub.TakeLatest();
  ASSERT_EQ(newest.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ((*newest).plan_id, kPublished);  // newest, not oldest
  // Conservation: one unique value seen + counted overwrites == published.
  EXPECT_EQ(1u + msg::introspect::OverwriteCount(sub), kPublished);

  // Every value is stamped (guarantee 4), and the age is truthful.
  EXPECT_GE(newest.age(), xmotion::Duration::zero());
  EXPECT_LT(newest.age(), 1s);

  // D6 warm start: a late joiner's first take yields the current value with
  // its ORIGINAL stamp — age never reports ~0 for old data. (Supports() is
  // consulted the way an application would, M6-A6; on a reach without the
  // contract this block self-disables as a stated divergence.)
  if (domain.Supports(msg::Contract::kLateJoinWarmStart)) {
    std::this_thread::sleep_for(50ms);
    auto late = domain.Subscribe<TrajectoryHead>(
        "m6.plan.head", {.history = msg::History::LatestOnly()});
    ASSERT_EQ(late.status(), msg::SubscribeStatus::kOk);
    auto warm = late.TakeLatest();
    ASSERT_EQ(warm.freshness(), msg::Freshness::kFresh);
    EXPECT_EQ((*warm).plan_id, kPublished);
    EXPECT_GE(warm.age(), 50ms);  // original stamp survived the warm start
    EXPECT_EQ(warm.stamp(), newest.stamp());
  }
}

// -- A5 + A6: clock story and support matrix are wiring-time facts ------------
TEST_P(M6ReachTransparency, SupportMatrixAndAgeClass) {
  auto domain = GetParam().make();

  // M6-A6 (R3, divergence over emulation): the Supports() answers must
  // match the documented per-reach matrix, contract by contract — a
  // divergence is wiring-time knowledge, never a field surprise.
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(domain.Supports(kAllContracts[i]), GetParam().supports[i])
        << "contract " << i << " on reach " << domain.reach_name();
  }

  // M6-A5 (R8/D12): the age a sample reports knows its own trustworthiness.
  auto pub = domain.Advertise<TrajectoryHead>(
      "m6.age.class", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m6.age.class", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);
  pub.Publish(TrajectoryHead{1, {}, {}});
  auto sample = sub.TakeLatest();
  ASSERT_EQ(sample.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ(sample.age_class(), GetParam().expected_age_class);

  // The reach identifies itself for logs and divergence messages.
  EXPECT_FALSE(domain.reach_name().empty());
}

// P1/P2: append the new reach's ReachCase here — the TEST_P bodies above
// must not change (M6-A1). P1b (PosixShm) appended without touching them.
#if defined(XMMESSAGING_HAS_POSIX_SHM)
INSTANTIATE_TEST_SUITE_P(Reaches, M6ReachTransparency,
                         ::testing::Values(kInProcessCase, kPosixShmCase),
                         [](const ::testing::TestParamInfo<ReachCase>& info) {
                           return std::string(info.param.name);
                         });
#else
INSTANTIATE_TEST_SUITE_P(Reaches, M6ReachTransparency,
                         ::testing::Values(kInProcessCase),
                         [](const ::testing::TestParamInfo<ReachCase>& info) {
                           return std::string(info.param.name);
                         });
#endif

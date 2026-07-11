/*
 * m11_behavioral_test.cpp — M11: rebuild skew, type-identity refusal (R6,
 * POSIX-shm reach). The field incident where two processes built from
 * different commits disagree about a payload's layout, made impossible to
 * hit silently.
 *
 * The skew is REAL: this test binary advertises the baseline M11Plan
 * (m11_payload.hpp, XMMSG_M11_VARIANT=0) and spawns three separately
 * compiled subscriber binaries, each built from the same source with a
 * divergent variant of the type — plus the baseline-built control binary.
 *
 * Acceptance criteria (docs/scenarios.md):
 *   A1 — all three skew cases (append / reorder / retype) are refused with
 *        the distinct kTypeMismatch status; the subscriber never receives
 *        a single reinterpreted byte (the helper exits nonzero if its
 *        subscribe was accepted).
 *   A2 — the reorder case (identical sizeof, identical names) is caught —
 *        the §4 hash covers layout, not name and size.
 *   A3 — the refusal is visible in introspection: the segment records both
 *        hashes (established + refused), read externally by the M10 reader
 *        AND rendered by the xmmsg CLI.
 *   A4 — the control case matches: a separately-compiled identical layout
 *        produces an identical hash (deterministic across builds); the
 *        expected hashes are recomputed here from the §4.2 canonical
 *        description strings VERBATIM, proving spec-computability.
 *   A5 — the refusal is local: a second topic between the same two
 *        processes flows while the first stays refused (asserted inside
 *        every helper run).
 *
 * Test conditions: inter-process (fork + exec of m11_helper_v{0..3}),
 * 2 ms publish cadence on both topics, latest-only QoS. TSan cannot see
 * cross-process shm races (shm_test_support.hpp); the suite also runs
 * under ASan.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "m11_payload.hpp"
#include "shm_test_support.hpp"
#include "xmmsg/messaging.hpp"

#if !defined(XMMESSAGING_HAS_POSIX_SHM)

TEST(M11Behavioral, DISABLED_RequiresPosixShmBackend) {
  GTEST_SKIP() << "XMMESSAGING_WITH_POSIX_SHM is off";
}

#else

#include "xmmsg/detail/introspect_reader.hpp"

namespace msg = xmotion::messaging;
namespace det = xmotion::messaging::detail;
using namespace std::chrono_literals;

namespace {

constexpr const char* kMainTopic = "m11.plan.head";
constexpr const char* kSideTopic = "m11.side.state";

// The §4.2 canonical descriptions of every variant, VERBATIM from the spec
// rules (one LF-terminated line per leaf field, name:type:offset:size,
// decimal, no whitespace). The test hashes these with the spec's FNV-1a-64
// and requires the separately-built binaries to have computed the same
// values — the M12-A2 "computable from the spec alone" property, exercised
// as part of M11.
constexpr const char* kBaselineDesc =
    "size:40\n"
    "plan_id:u64:0:8\n"
    "x:f64:8:8\n"
    "y:f64:16:8\n"
    "theta:f64:24:8\n"
    "tick:u64:32:8\n";
constexpr const char* kAppendedDesc =  // variant 1: field appended
    "size:48\n"
    "plan_id:u64:0:8\n"
    "x:f64:8:8\n"
    "y:f64:16:8\n"
    "theta:f64:24:8\n"
    "tick:u64:32:8\n"
    "extra:f64:40:8\n";
constexpr const char* kReorderDesc =  // variant 2: y/theta swapped (A2)
    "size:40\n"
    "plan_id:u64:0:8\n"
    "x:f64:8:8\n"
    "theta:f64:16:8\n"
    "y:f64:24:8\n"
    "tick:u64:32:8\n";
constexpr const char* kRetypeDesc =  // variant 3: theta f64 -> u64
    "size:40\n"
    "plan_id:u64:0:8\n"
    "x:f64:8:8\n"
    "y:f64:16:8\n"
    "theta:u64:24:8\n"
    "tick:u64:32:8\n";

// Wiring fixture: baseline publisher on both topics, helper run to
// completion while the parent keeps both topics fed (the helpers wait,
// bounded, for fresh values).
struct M11Fixture {
  explicit M11Fixture(const char* tag)
      : domain_name(shmtest::UniqueDomainName(tag)),
        janitor(domain_name, {kMainTopic, kSideTopic}),
        domain(msg::Domain::PosixShm({.name = domain_name})),
        main_pub(domain.Advertise<M11Plan>(
            kMainTopic, {.history = msg::History::LatestOnly()})),
        side_pub(domain.Advertise<ShmTestPlan>(
            kSideTopic, {.history = msg::History::LatestOnly()})) {}

  bool Ok() const {
    return main_pub.status() == msg::AdvertiseStatus::kOk &&
           side_pub.status() == msg::AdvertiseStatus::kOk;
  }

  // Spawn the helper and publish on BOTH topics at a 2 ms cadence until it
  // exits (bounded); returns the helper's exit code (or -signal / -1000).
  int RunHelper(const char* helper_path, const char* expectation) {
    const pid_t pid = shmtest::SpawnBinary(
        helper_path, {expectation, domain_name, kMainTopic, kSideTopic});
    if (pid <= 0) {
      return -1000;
    }
    shmtest::ChildGuard guard(pid);
    const auto deadline = xmotion::Now() + 30s;
    std::uint64_t i = 0;
    int status = 0;
    for (;;) {
      M11Plan plan{};
      plan.plan_id = ++i;
      plan.x = 1.0;
      plan.y = 2.0;
      plan.theta = 0.5;
      plan.tick = i;
      main_pub.Publish(plan);
      ShmTestPlan side{};
      FillShmPlan(side, i);
      side_pub.Publish(side);
      const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
      if (reaped == pid) {
        break;
      }
      if (xmotion::Now() >= deadline) {
        return -2000;  // helper hung — ChildGuard kills it
      }
      std::this_thread::sleep_for(2ms);
    }
    guard.Reap();  // already reaped by WNOHANG: disarm the guard's kill
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return WIFSIGNALED(status) ? -WTERMSIG(status) : -1000;
  }

  // A3: the refusal record, read externally through the M10 reader.
  bool SnapshotMainTopic(det::IntrospectSnapshot* snap) {
    const std::string key = det::DeriveIsolationKey(domain_name);
    det::IntrospectReader reader;
    if (det::IntrospectReader::Open(det::ShmSegmentName(key, kMainTopic),
                                    &reader) !=
        det::IntrospectOpenStatus::kOk) {
      return false;
    }
    return reader.Snapshot(snap);
  }

  const std::string domain_name;
  shmtest::SegmentJanitor janitor;
  msg::Domain domain;
  msg::Publisher<M11Plan> main_pub;
  msg::Publisher<ShmTestPlan> side_pub;
};

// Shared skew-case body: run the variant helper expecting refusal, then
// assert the A3 refusal record carries BOTH hashes — the established
// baseline hash and the variant hash, each recomputed here from its §4.2
// canonical string.
void ExpectSkewRefused(const char* tag, const char* helper_path,
                       const char* variant_desc,
                       std::uint64_t variant_payload_size) {
  M11Fixture fixture(tag);
  ASSERT_TRUE(fixture.Ok());

  // A1/A2 + A5: refused with the distinct status (exit 0 == expectation
  // met AND the side topic flowed; every failure mode has its own code).
  EXPECT_EQ(fixture.RunHelper(helper_path, "expect_refusal"), 0);

  // A3: both hashes visible externally.
  det::IntrospectSnapshot snap;
  ASSERT_TRUE(fixture.SnapshotMainTopic(&snap));
  EXPECT_EQ(snap.schema_hash, det::Fnv1a64(kBaselineDesc))
      << "the topic's established identity must be the baseline hash";
  EXPECT_GE(snap.refusal_count, 1u) << "the refusal must be recorded";
  EXPECT_EQ(snap.refused_schema_hash, det::Fnv1a64(variant_desc))
      << "the refused endpoint's hash must be shown, and it must equal the "
         "hash computed from the spec's canonical description alone";
  EXPECT_NE(snap.refused_schema_hash, snap.schema_hash);
  EXPECT_EQ(snap.refused_payload_size, variant_payload_size);

  // A5, parent side: the side topic still has a live publisher and the
  // segment took no refusals — the refusal stayed local to kMainTopic.
  const std::string key = det::DeriveIsolationKey(fixture.domain_name);
  det::IntrospectReader side_reader;
  ASSERT_EQ(det::IntrospectReader::Open(
                det::ShmSegmentName(key, kSideTopic), &side_reader),
            det::IntrospectOpenStatus::kOk);
  det::IntrospectSnapshot side_snap;
  ASSERT_TRUE(side_reader.Snapshot(&side_snap));
  EXPECT_EQ(side_snap.refusal_count, 0u);
}

}  // namespace

// A1 case 1: field appended — size change.
TEST(M11Behavioral, AppendedFieldRefused) {
  ExpectSkewRefused("m11a", XMMSG_M11_HELPER_V1, kAppendedDesc, 48);
}

// A2: two fields reordered at identical sizeof and identical names — the
// nasty case; only the §4.2 offsets distinguish the layouts.
TEST(M11Behavioral, ReorderAtSameSizeRefused) {
  ExpectSkewRefused("m11b", XMMSG_M11_HELPER_V2, kReorderDesc, 40);
}

// A1 case 3: type changed at same offset/size (f64 -> u64).
TEST(M11Behavioral, TypeChangeAtSameOffsetRefused) {
  ExpectSkewRefused("m11c", XMMSG_M11_HELPER_V3, kRetypeDesc, 40);
}

// A4: the control case — the baseline helper is a SEPARATELY COMPILED
// binary carrying an identical layout; it must match and flow. Plus the
// spec-determinism cross-check: this process's compile-time-derived hash
// equals the hash of the §4.2 canonical string written out by hand.
TEST(M11Behavioral, IdenticalLayoutAcrossBuildsMatches) {
  EXPECT_EQ(det::SchemaHashOf<M11Plan>(), det::Fnv1a64(kBaselineDesc))
      << "the canonical description generator must reproduce the spec "
         "string exactly (M11-A4/M12-A2)";

  M11Fixture fixture("m11d");
  ASSERT_TRUE(fixture.Ok());
  EXPECT_EQ(fixture.RunHelper(XMMSG_M11_HELPER_V0, "expect_match"), 0);

  det::IntrospectSnapshot snap;
  ASSERT_TRUE(fixture.SnapshotMainTopic(&snap));
  EXPECT_EQ(snap.schema_hash, det::Fnv1a64(kBaselineDesc));
  EXPECT_EQ(snap.refusal_count, 0u) << "the control case must not refuse";
}

// A3, CLI leg: the refusal must be diagnosable from the xmmsg tool's
// output alone — both hashes rendered, the mismatch named.
#if defined(XMMSG_CLI_PATH)
TEST(M11Behavioral, RefusalVisibleInCli) {
  M11Fixture fixture("m11e");
  ASSERT_TRUE(fixture.Ok());
  ASSERT_EQ(fixture.RunHelper(XMMSG_M11_HELPER_V2, "expect_refusal"), 0);

  const std::string key = det::DeriveIsolationKey(fixture.domain_name);
  const std::string base = std::string(XMMSG_CLI_PATH) + " stat " +
                           kMainTopic + " --domain " + key;
  std::string plain;
  ASSERT_EQ(shmtest::RunCommandCaptureStdout(base, &plain), 0);
  char expected_hash[32];
  std::snprintf(expected_hash, sizeof(expected_hash), "0x%016llX",
                static_cast<unsigned long long>(det::Fnv1a64(kReorderDesc)));
  char established_hash[32];
  std::snprintf(established_hash, sizeof(established_hash), "0x%016llX",
                static_cast<unsigned long long>(det::Fnv1a64(kBaselineDesc)));
  EXPECT_NE(plain.find("TYPE MISMATCH"), std::string::npos) << plain;
  EXPECT_NE(plain.find(expected_hash), std::string::npos)
      << "refused hash missing from CLI output:\n"
      << plain;
  EXPECT_NE(plain.find(established_hash), std::string::npos)
      << "established hash missing from CLI output:\n"
      << plain;

  std::string json;
  ASSERT_EQ(shmtest::RunCommandCaptureStdout(base + " --json", &json), 0);
  EXPECT_TRUE(shmtest::JsonChecker::Parses(json)) << json;
  EXPECT_NE(json.find("\"last_schema_hash\":\""), std::string::npos) << json;
  EXPECT_NE(json.find(expected_hash), std::string::npos) << json;
}
#endif  // XMMSG_CLI_PATH

#endif  // XMMESSAGING_HAS_POSIX_SHM

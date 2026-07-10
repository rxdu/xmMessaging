/*
 * wire_schema_test.cpp — R6/M11 groundwork (P1b): the XMMSG_DESCRIBE
 * canonical schema description must reproduce the wire-contract §5
 * conformance vectors EXACTLY — hash and, for the baseline vector, the
 * canonical string byte-for-byte. This is the gate that lets a schema hash
 * cross a process boundary (the shm segment header carries it): the
 * canonical form is a pure function of the declared wire layout, never of
 * the build.
 *
 * Also pinned here: the interim (typeid-based) fallback hash can never
 * collide with a canonical hash by accident of format, and the M11-A2
 * reorder case is caught by the canonical form.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "xmmessaging/messaging.hpp"

namespace detail = xmotion::messaging::detail;

// ---- §5 vector layouts, declared exactly as specified ----------------------
// (Global scope: XMMSG_DESCRIBE opens the library namespace.)

struct V1Pose2d {  // 24 bytes, no padding needed
  double x;
  double y;
  double theta;
};
XMMSG_DESCRIBE(V1Pose2d, XMMSG_FIELD(x), XMMSG_FIELD(y), XMMSG_FIELD(theta));

struct V2Pose2dReordered {  // still 24 bytes; y and theta swapped (M11-A2)
  double x;
  double theta;
  double y;
};
XMMSG_DESCRIBE(V2Pose2dReordered, XMMSG_FIELD(x), XMMSG_FIELD(theta),
               XMMSG_FIELD(y));

struct V3Pose2dTypeSwap {  // theta became a raw tick count (u64)
  double x;
  double y;
  std::uint64_t theta;
};
XMMSG_DESCRIBE(V3Pose2dTypeSwap, XMMSG_FIELD(x), XMMSG_FIELD(y),
               XMMSG_FIELD(theta));

struct V4StampedPose2d {  // 32 bytes; nested struct expands, no line of its own
  std::uint64_t stamp;
  V1Pose2d pose;
};
XMMSG_DESCRIBE(V4StampedPose2d, XMMSG_FIELD(stamp), XMMSG_FIELD(pose));

struct V5ImuSample {  // 32 bytes; fixed scalar arrays are one line each
  std::uint64_t stamp;
  float accel[3];
  float gyro[3];
};
XMMSG_DESCRIBE(V5ImuSample, XMMSG_FIELD(stamp), XMMSG_FIELD(accel),
               XMMSG_FIELD(gyro));

struct V6ModeStatus {  // 8 bytes; padding made explicit (§3), participates
  std::uint8_t mode;
  std::uint8_t _pad0[3];
  std::uint32_t code;
};
XMMSG_DESCRIBE(V6ModeStatus, XMMSG_FIELD(mode), XMMSG_FIELD(_pad0),
               XMMSG_FIELD(code));

namespace {

TEST(WireSchema, HashFunctionReferenceVectors) {
  // §4.1 cross-checks for the FNV-1a-64 function itself.
  EXPECT_EQ(detail::Fnv1a64(""), 0xCBF29CE484222325ULL);
  EXPECT_EQ(detail::Fnv1a64("a"), 0xAF63DC4C8601EC8CULL);
  EXPECT_EQ(detail::Fnv1a64("foobar"), 0x85944171F73967E8ULL);
}

TEST(WireSchema, V1BaselineCanonicalStringIsExact) {
  // §4.2 is whitespace-inflexible: the string must match byte-for-byte,
  // every line LF-terminated including the last.
  EXPECT_EQ(detail::CanonicalDescription<V1Pose2d>(),
            "size:24\n"
            "x:f64:0:8\n"
            "y:f64:8:8\n"
            "theta:f64:16:8\n");
}

TEST(WireSchema, ConformanceVectorsV1toV6) {
  EXPECT_EQ(detail::SchemaHashOf<V1Pose2d>(), 0xE0978597FA5660D4ULL);
  EXPECT_EQ(detail::SchemaHashOf<V2Pose2dReordered>(), 0x9EF4520D5E058D58ULL);
  EXPECT_EQ(detail::SchemaHashOf<V3Pose2dTypeSwap>(), 0x9444641EFF1DE2E1ULL);
  EXPECT_EQ(detail::SchemaHashOf<V4StampedPose2d>(), 0x777F5FA85CE9182AULL);
  EXPECT_EQ(detail::SchemaHashOf<V5ImuSample>(), 0x4F932BBE69B94D14ULL);
  EXPECT_EQ(detail::SchemaHashOf<V6ModeStatus>(), 0x5F05885505F5CDDCULL);
}

TEST(WireSchema, SkewCasesRefuseByHash) {
  // M11's three skew cases, at the hash level: reorder at identical size
  // (A2, the nasty one), and type swap at identical offsets/sizes/names.
  EXPECT_NE(detail::SchemaHashOf<V1Pose2d>(),
            detail::SchemaHashOf<V2Pose2dReordered>());
  EXPECT_NE(detail::SchemaHashOf<V1Pose2d>(),
            detail::SchemaHashOf<V3Pose2dTypeSwap>());
}

// An undescribed type shaped exactly like V1: it falls back to the interim
// (typeid) hash, which the "xmmsg-interim-schema:" prefix keeps disjoint
// from every canonical hash by construction — a described endpoint can
// never accidentally match an undescribed one.
struct UndescribedPose2d {
  double x;
  double y;
  double theta;
};

TEST(WireSchema, InterimFallbackNeverCollidesWithCanonical) {
  EXPECT_NE(detail::SchemaHashOf<UndescribedPose2d>(),
            detail::SchemaHashOf<V1Pose2d>());
  // And the interim hash is still deterministic within a build.
  EXPECT_EQ(detail::SchemaHashOf<UndescribedPose2d>(),
            detail::SchemaHashOf<UndescribedPose2d>());
}

}  // namespace

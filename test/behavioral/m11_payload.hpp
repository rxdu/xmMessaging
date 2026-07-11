/*
 * m11_payload.hpp — the deliberately skewed payload family for M11 (rebuild
 * skew: type-identity refusal). ONE source of truth, compiled into FOUR
 * separate binaries via -DXMMSG_M11_VARIANT=<n> — exactly the field
 * incident R6 exists for: processes built from divergent definitions of
 * "the same" type.
 *
 *   0  baseline        plan_id,x,y,theta,tick        40 B  (the M1-shaped
 *                      TrajectoryHead stand-in; also the M11-A4 control:
 *                      a separately-compiled identical layout MUST match)
 *   1  field appended  + extra                       48 B  (size change)
 *   2  reorder         y and theta swapped           40 B  (same size, same
 *                      names, same sizeof — the M11-A2 nasty case; only
 *                      the §4.2 offsets distinguish it)
 *   3  type change     theta f64 -> u64              40 B  (same offset,
 *                      same size; only the §4.2 type token distinguishes)
 *
 * All variants are explicitly padded §3-conforming PODs opted into the
 * canonical §4 hash via XMMSG_DESCRIBE (fields listed in declaration
 * order). The expected canonical description strings — and therefore the
 * expected hashes — are restated verbatim in m11_behavioral_test.cpp, so
 * the test also proves the hash is computable from the spec alone.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>

#include "xmmessaging/messaging.hpp"

#ifndef XMMSG_M11_VARIANT
#define XMMSG_M11_VARIANT 0  // baseline (the test binary compiles this)
#endif

struct M11Plan {
  std::uint64_t plan_id;  // offset 0
#if XMMSG_M11_VARIANT == 2
  double x;             // offset 8
  double theta;         // offset 16  <- was y (the reorder)
  double y;             // offset 24  <- was theta
#elif XMMSG_M11_VARIANT == 3
  double x;             // offset 8
  double y;             // offset 16
  std::uint64_t theta;  // offset 24  <- was f64 (same offset, same size)
#else
  double x;      // offset 8
  double y;      // offset 16
  double theta;  // offset 24
#endif
  std::uint64_t tick;  // offset 32
#if XMMSG_M11_VARIANT == 1
  double extra;  // offset 40 — appended field (size change)
#endif
};

#if XMMSG_M11_VARIANT == 1
static_assert(sizeof(M11Plan) == 48, "variant 1 appends one f64");
XMMSG_DESCRIBE(M11Plan, XMMSG_FIELD(plan_id), XMMSG_FIELD(x), XMMSG_FIELD(y),
               XMMSG_FIELD(theta), XMMSG_FIELD(tick), XMMSG_FIELD(extra));
#elif XMMSG_M11_VARIANT == 2
static_assert(sizeof(M11Plan) == 40, "variant 2 is a pure reorder");
XMMSG_DESCRIBE(M11Plan, XMMSG_FIELD(plan_id), XMMSG_FIELD(x),
               XMMSG_FIELD(theta), XMMSG_FIELD(y), XMMSG_FIELD(tick));
#else
static_assert(sizeof(M11Plan) == 40, "baseline/variant-3 layout");
XMMSG_DESCRIBE(M11Plan, XMMSG_FIELD(plan_id), XMMSG_FIELD(x), XMMSG_FIELD(y),
               XMMSG_FIELD(theta), XMMSG_FIELD(tick));
#endif

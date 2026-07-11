/*
 * status.hpp
 *
 * The status vocabulary. R2: every failure mode a user can hit has a
 * distinct, documented status — APIs are confusing at the error surface,
 * not the happy path. D18: endpoint construction never throws; handles
 * carry a queryable status, so a launcher can enumerate exactly what
 * failed. There are no exceptions anywhere on this surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>

namespace xmotion {
namespace messaging {

// Result of Publisher::Publish / Publisher::PublishDerived.
enum class PublishStatus : std::uint8_t {
  // The transport accepted the value. Best-effort Publish always returns
  // kOk — per-subscriber overflow drops land in the subscribers' own
  // counters, because delivered-to-whom is an endpoint-local truth (D8).
  kOk,
  // D8: reliable + bounded queue full RIGHT NOW; nothing was enqueued.
  // Publish never blocks internally — retry/coalesce/shed is caller policy.
  // Never occurs on latest-only (the slot is overwritten, M1-A1).
  kWouldBlock,
};

// Status carried by a Loan handle (queryable, D18 pattern). Loan exhaustion
// is a Loan()-time fact, not a Publish status: design.md's "loan-exhausted"
// surfaces here so PublishStatus stays exactly {kOk, kWouldBlock} (D8).
enum class LoanStatus : std::uint8_t {
  kOk,
  // The loan pool declared at wiring time is exhausted; publishing an
  // invalid loan is a contract violation (debug-assert, mirrors D2).
  kExhausted,
};

// Status carried by a Publisher handle (D18: construction never throws).
enum class AdvertiseStatus : std::uint8_t {
  kOk,
  // D15/D18: the topic already has an exclusive publisher — an accidentally
  // duplicated node cannot silently fight the real one (M14-A3).
  kOwnershipRefused,
  // R6: the topic exists with a different payload schema hash; the match is
  // refused, visible in introspection (M11).
  kTypeMismatch,
  // M8-A2: the domain's reach is not compiled into this build — never a
  // silent in-process fallback.
  kUnsupportedReach,
};

// Status carried by a Subscriber handle (D18).
enum class SubscribeStatus : std::uint8_t {
  kOk,
  // R6: schema-hash mismatch against the topic's established type (M11).
  kTypeMismatch,
  // M8-A2: reach not compiled into this build.
  kUnsupportedReach,
};

// Result of Client::Call (D11): kDeadlineExpired and kNoServer are distinct
// — "server overloaded" and "server crashed" are different incidents with
// different recoveries (M5-A2/A3).
enum class CallStatus : std::uint8_t {
  kOk,
  // The deadline passed; the late reply is discarded by correlation and
  // never surfaces on a later call (M5-A2).
  kDeadlineExpired,
  // Nobody serves this topic — fails fast, distinct from timeout (M5-A3).
  kNoServer,
};

// Result of Server::Reply.
enum class ReplyStatus : std::uint8_t {
  kOk,
  // The caller's deadline already expired; the reply was discarded by
  // correlation (M5-A2) — observable server-side, never an error path.
  kExpired,
};

// Result of Domain::WaitUntilMatched (D16): the one bounded barrier verb.
enum class WaitStatus : std::uint8_t {
  // Every listed endpoint has at least one peer (M14-A2).
  kMatched,
  // The deadline passed with at least one endpoint unmatched — distinct so
  // "stack didn't come up" never looks like success or hangs (M14-A2);
  // which endpoint is missing is queryable via MatchedCount().
  kDeadlineExpired,
};

}  // namespace messaging
}  // namespace xmotion

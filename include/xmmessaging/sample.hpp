/*
 * sample.hpp
 *
 * Sample<T> — the take-side surface (D1): value + monotonic stamp +
 * freshness verdict, never an optional. Emptiness is a freshness state,
 * not a different return type, so the call site cannot skip handling it.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cassert>
#include <cstdint>

#include "xmbase/telemetry/context.hpp"
#include "xmmessaging/detail/envelope.hpp"  // the wire contract behind these fields
#include "xmmessaging/qos.hpp"

namespace xmotion {
namespace messaging {

namespace detail {
template <typename T>
class ShmSubImpl;  // P1b: the POSIX-shm take machinery mints Samples too
}  // namespace detail

// D2: tri-state freshness, judged by the library against the deadline
// declared at wiring time (one declaration, two surfaces — D3).
enum class Freshness : std::uint8_t {
  // Newest value, within the declared deadline (or no deadline declared).
  kFresh,
  // A value exists but exceeded the deadline — still accessible; the
  // Fresh->Stale transition also fires a messaging.* deadline-miss event.
  kStale,
  // Never received. Distinct from kStale: "never had a plan" and "lost the
  // planner" are different incidents (M1). Dereference is a contract
  // violation (debug-assert).
  kNone,
};

// D12 (R8): whether an age is a measurement or a hint. Same-host reaches
// and declared-synced clock domains yield kMeasured; inter-host without a
// ClockDomain declaration yields kAdvisory — and an advisory age NEVER
// produces a kStale verdict (unsynced clocks must not manufacture
// confidently-wrong staleness).
enum class AgeClass : std::uint8_t { kMeasured, kAdvisory };

// The value a take verb hands back. Wait-free and allocation-free to obtain
// on the hot path (R7). Consumer-side zero-copy View() was declined (D4);
// the Sample owns its copy of the payload.
template <typename T>
class Sample {
 public:
  // D1/D2: the library's verdict for this take.
  Freshness freshness() const noexcept { return freshness_; }

  // Last-hop age: now - publish stamp (R8, CLOCK_MONOTONIC). Valid for
  // custom staleness logic regardless of the declared deadline.
  Duration age() const noexcept { return ::xmotion::Now() - stamp_; }

  // D14: information age: now - origin stamp — "how old is the sensor data
  // underneath this value", not "how old is this value" (M13-A1).
  Duration origin_age() const noexcept {
    return ::xmotion::Now() - origin_stamp_;
  }

  // D12 (R8): whether age()/origin_age() are measurements or advisory.
  AgeClass age_class() const noexcept { return age_class_; }

  // Publish stamp on the family monotonic clock. A late-join warm-start
  // value keeps its ORIGINAL stamp — age never reports ~0 for old data (D6).
  Timestamp stamp() const noexcept { return stamp_; }

  // D14: origin stamp — == stamp() for a first-hop publish (M13-A2).
  Timestamp origin_stamp() const noexcept { return origin_stamp_; }

  // D14: hops since origin; 0 for a first-hop publish.
  std::uint32_t hop_count() const noexcept { return hop_count_; }

  // D13: the publisher's telemetry context, extracted from the envelope.
  // Adoption is explicit — wrap in a telemetry ContextGuard; automatic
  // adoption would silently reparent the consumer's own trace (M7-A2).
  ::xmotion::telemetry::Context context() const noexcept { return context_; }

  // D2: dereferencing a kNone sample is a contract violation (debug-assert).
  const T& operator*() const noexcept {
    assert(freshness_ != Freshness::kNone);
    return value_;
  }
  const T* operator->() const noexcept {
    assert(freshness_ != Freshness::kNone);
    return &value_;
  }

 private:
  template <typename>
  friend class Subscriber;  // samples are minted by take verbs only
  template <typename>
  friend class detail::ShmSubImpl;  // P1b: the shm take verbs (same rule)

  T value_{};
  ::xmotion::telemetry::Context context_{};
  Timestamp stamp_{};
  Timestamp origin_stamp_{};
  std::uint32_t hop_count_ = 0;
  Freshness freshness_ = Freshness::kNone;
  AgeClass age_class_ = AgeClass::kMeasured;
};

}  // namespace messaging
}  // namespace xmotion

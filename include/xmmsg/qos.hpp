/*
 * qos.hpp
 *
 * The five-knob QoS vocabulary (design.md "The QoS vocabulary", ADR 0006):
 * History, Reliability, Deadline, Loan, Ownership — the only knobs, each
 * with a defined meaning in all three reaches. The Loan knob is a verb
 * (Publisher::Loan, endpoints.hpp), not a field here.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>
#include <optional>

#include "xmbase/types/time.hpp"

namespace xmotion {
namespace messaging {

// One time vocabulary for the whole family (design.md "One clock", R8/R11):
// messaging stamps and telemetry records share the xmBase monotonic clock,
// so transport hops and application spans interleave on one timeline by
// construction. Same types, not parallel ones.
using Clock = ::xmotion::Clock;          // steady_clock — monotonic
using Timestamp = ::xmotion::Timestamp;  // ns-resolution monotonic time point
using Duration = ::xmotion::Duration;    // nanoseconds

// History knob: depth-1 slot (latest-only) or bounded FIFO (queue<N>).
class History {
 public:
  enum class Kind : std::uint8_t { kLatestOnly, kQueue };

  // The LatestMailbox contract (design.md): a new value overwrites the
  // unread old one; Publish never blocks or fails for capacity reasons;
  // overwritten-unread values are counted; every value is stamped.
  static constexpr History LatestOnly() noexcept {
    return History(Kind::kLatestOnly, 1);
  }

  // Bounded FIFO of the given depth. All transport memory is sized at
  // wiring time from this declaration (R7 — nothing grows unbounded).
  static constexpr History Queue(std::uint32_t depth) noexcept {
    return History(Kind::kQueue, depth);
  }

  // Default history is latest-only — the robotics workhorse.
  constexpr History() noexcept = default;

  constexpr Kind kind() const noexcept { return kind_; }
  constexpr std::uint32_t depth() const noexcept { return depth_; }

 private:
  constexpr History(Kind kind, std::uint32_t depth) noexcept
      : kind_(kind), depth_(depth) {}

  Kind kind_ = Kind::kLatestOnly;
  std::uint32_t depth_ = 1;
};

// Reliability knob (design.md "Back-pressure is explicit", D8): best-effort
// overflow drops are per-subscriber facts, counted, never silent; reliable
// overflow back-pressures the publisher via PublishStatus::kWouldBlock.
enum class Reliability : std::uint8_t { kBestEffort, kReliable };

// Ownership knob (D15): kExclusive (default) — a second Advertise on the
// topic is refused with AdvertiseStatus::kOwnershipRefused; kShared is a
// deliberate declaration made by BOTH publishers — latest-only resolves to
// last-writer-wins by publish stamp, documented and deterministic.
enum class Ownership : std::uint8_t { kExclusive, kShared };

// The per-endpoint QoS declaration, passed at Advertise/Subscribe time.
// Aggregate by design: wiring code names only the knobs it turns
// (designated initializers), everything else keeps the documented default.
struct Qos {
  // History applies to both ends; each subscriber owns an independent
  // mailbox/queue of this shape (D7).
  History history{};

  // Publisher-side overflow policy (meaningful for queue<N>; latest-only
  // never refuses for capacity — the slot is overwritten).
  Reliability reliability = Reliability::kBestEffort;

  // Deadline knob (D2/D3): the consumer-side staleness bound, judged by the
  // library per take against this one declaration — the per-take Freshness
  // verdict and the messaging.* deadline-miss event agree by construction.
  // No deadline declared -> kStale never applies (M2).
  std::optional<Duration> deadline{};

  // Publisher-side duplicate-advertise policy (D15).
  Ownership ownership = Ownership::kExclusive;
};

}  // namespace messaging
}  // namespace xmotion

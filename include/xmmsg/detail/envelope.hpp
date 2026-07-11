/*
 * detail/envelope.hpp
 *
 * The envelope contract (design.md "The envelope contract", R10/R11):
 * every message carries a fixed-size header — the xmBase telemetry context
 * bytes (Inject/Extract), the publish stamp, and the information lineage
 * (origin stamp + hop count, D14). The wire layout is a fixed-endian,
 * fixed-offset documented byte structure (R10), asserted here so drift is
 * a compile error; the language-neutral byte-level spec is published
 * separately as the interop surface.
 *
 * detail/: not part of the portable API surface. Applications never name
 * these types; Sample<T> (sample.hpp) is the read surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "xmbase/telemetry/context.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// Fixed size either way (M7-A3): with no active trace the context field is
// the null context — valid bytes, never garbage, identical envelope size.
inline constexpr std::size_t kEnvelopeContextSize =
    ::xmotion::telemetry::kContextWireSize;  // 24: trace.hi | trace.lo | span

// The per-message header. Per-hop decomposition is deliberately NOT carried
// (it would break the fixed-size contract); it is reconstructed offline
// from the M7 trace links (D14).
struct Envelope {
  // D13: Publish snapshots the calling thread's active telemetry context
  // (xmBase Inject bytes); the take side adopts explicitly via a
  // ContextGuard over Sample::context(). The context travels with the
  // value — an overwritten latest-only value takes its context with it.
  std::uint8_t context[kEnvelopeContextSize];
  // Publish time on the xmBase monotonic clock, nanoseconds (R8).
  std::int64_t publish_stamp_ns;
  // D14: origin stamp — the oldest consumed input's origin for a derived
  // value, == publish_stamp_ns for a first-hop publish (M13-A2).
  std::int64_t origin_stamp_ns;
  // D14: hops since origin; 0 for a first-hop publish.
  std::uint32_t hop_count;
  // Explicit padding/flags (R10: no implicit padding bytes on the wire).
  std::uint32_t flags;
};

static_assert(std::is_trivially_copyable_v<Envelope>,
              "envelope must be raw-copyable into any backend's frame");
static_assert(sizeof(Envelope) == 48,
              "the envelope is a fixed-size contract (M7-A3): 24B context + "
              "8B stamp + 8B origin + 4B hops + 4B flags");
static_assert(offsetof(Envelope, publish_stamp_ns) == 24 &&
                  offsetof(Envelope, origin_stamp_ns) == 32 &&
                  offsetof(Envelope, hop_count) == 40 &&
                  offsetof(Envelope, flags) == 44,
              "envelope offsets are a published wire contract (R10)");

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

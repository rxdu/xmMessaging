/*
 * detail/mail_record.hpp
 *
 * MailRecord<T> — the unit every transport cell carries: the fixed 48-byte
 * Envelope (detail/envelope.hpp), a per-topic publish ordinal, and the
 * payload. The ordinal is a TRANSPORT-LOCAL fact (in-process accounting for
 * the LatestMailbox overwrite counter, guarantee 3), not an envelope field:
 * it never crosses the wire, so it lives beside the envelope, not in it.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>

#include "xmmessaging/detail/envelope.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

template <typename T>
struct MailRecord {
  Envelope envelope{};  // zero context / stamps until first Store
  // 1-based ordinal of the ACCEPTED publish on this topic (0 = never
  // written). Ordinals are contiguous over accepted publishes — a reliable
  // kWouldBlock refusal consumes no ordinal — which is what makes the
  // per-subscriber overwrite accounting exact (see in_process.hpp).
  std::uint64_t ordinal = 0;
  T payload{};
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

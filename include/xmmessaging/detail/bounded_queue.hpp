/*
 * detail/bounded_queue.hpp
 *
 * BoundedQueue<T, Placement> — the lock-free bounded ring behind
 * history=queue<N> (R7: lock-free, allocation-free after wiring, memory
 * sized at wiring from the declared depth).
 *
 * Concurrency contract: SPSC — one producer, one consumer. This matches
 * the transport's shape by construction: each SUBSCRIBER owns its ring
 * (D7/D8), its owner is the only consumer, and the topic's exclusive
 * publisher (D15 default) is the only producer. Shared-ownership topics
 * (two+ declared publishers) serialize the producer side with the topic's
 * writer mutex one level up (in_process.hpp) — a deliberate P0b interim:
 *
 *   TODO(P1): lock-free MPMC (or at least MPSC producer side) upgrade for
 *   shared-ownership queues. The exclusive-ownership path never takes a
 *   lock — the mutex exists ONLY on the declared-shared path, so the
 *   default hot path does not silently ship a lock.
 *
 * Memory-ordering pairs, referenced from the code:
 *   (Q1) producer: head_.load(acquire)  — pairs with (Q2), the consumer's
 *        head_.store(release) after it copied a cell out: the producer may
 *        only reuse a cell after it has observed the consumer's release,
 *        which orders the consumer's plain copy-out before the producer's
 *        plain overwrite (no data race on cell reuse).
 *   (Q3) producer: tail_.store(release) after the plain cell write — pairs
 *        with (Q4), the consumer's tail_.load(acquire): a consumer that
 *        observes the new tail observes the fully-written cell (no torn
 *        reads; cells need no atomics, unlike the seqlock slot).
 *
 * Overflow policy lives with the caller (in_process.hpp): reliable
 * publishers pre-check Full() and refuse with kWouldBlock (nothing
 * enqueued); best-effort publishers drop the INCOMING value ("drop
 * newest") and count the drop on the owning subscriber (D8).
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xmmessaging/detail/mail_record.hpp"
#include "xmmessaging/detail/placement.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

template <typename T, typename Placement = HeapPlacement>
class BoundedQueue {
 public:
  using Record = MailRecord<T>;

  // Depth comes from the wiring-time QoS declaration (History::Queue(N));
  // all cell memory is allocated here, never on the publish/take path (R7).
  explicit BoundedQueue(std::uint32_t depth)
      : capacity_(depth == 0 ? 1 : depth),
        cells_(Placement::template MakeArray<Record>(capacity_)) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Producer side. Returns false when full (caller applies the QoS policy).
  bool TryPush(const Record& record) {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);  // (Q1)
    if (tail - head >= capacity_) {
      return false;
    }
    cells_[tail % capacity_] = record;              // plain write, see (Q3)
    tail_.store(tail + 1, std::memory_order_release);                // (Q3)
    return true;
  }

  // Consumer side. Returns false when empty.
  bool TryPop(Record& out) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);  // (Q4)
    if (head == tail) {
      return false;
    }
    out = cells_[head % capacity_];                 // plain read, see (Q1)
    head_.store(head + 1, std::memory_order_release);                // (Q2)
    return true;
  }

  // Producer-side fullness check (exact from the producer thread: only the
  // consumer can make it less full between this check and TryPush).
  bool Full() const noexcept {
    return tail_.load(std::memory_order_relaxed) -
               head_.load(std::memory_order_acquire) >=
           capacity_;
  }

  // Advisory size (for the queue_depth gauge).
  std::size_t Size() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
  }

  std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(capacity_);
  }

 private:
  const std::uint64_t capacity_;
  typename Placement::template ArrayHandle<Record> cells_;
  // Separate cache lines: producer writes tail_, consumer writes head_.
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

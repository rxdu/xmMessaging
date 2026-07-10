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

  // Ring control block. Placed like the cells (heap or shared mapping,
  // P1b): producer and consumer may live in different PROCESSES, so the
  // indices — and the capacity, which the initializing side (the consumer,
  // who declared the QoS depth) authors — must live in shared memory, not
  // as members of this per-process view object.
  struct Control {
    std::atomic<std::uint64_t> capacity;
    // Separate cache lines: producer writes tail, consumer writes head.
    alignas(64) std::atomic<std::uint64_t> head;
    alignas(64) std::atomic<std::uint64_t> tail;
  };
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "shm-capable ring requires lock-free (address-free) 64-bit "
                "atomics");

  // Placed-storage sizes for the P1b segment-layout computation.
  static constexpr std::size_t ControlBytes() noexcept {
    return sizeof(Control);
  }
  static constexpr std::size_t RecordBytes() noexcept {
    return sizeof(Record);
  }

  // Depth comes from the wiring-time QoS declaration (History::Queue(N));
  // all cell memory is allocated/claimed here, never on the publish/take
  // path (R7). Only an INITIALIZING placement authors capacity/indices; an
  // attaching view (P1b: the publisher's view of a subscriber-owned ring)
  // passes the region's cell bound as `depth` and reads the live capacity
  // from the shared control block.
  explicit BoundedQueue(std::uint32_t depth, Placement placement = Placement())
      : ctrl_(placement.template MakeSingle<Control>()),
        cells_(placement.template MakeArray<Record>(depth == 0 ? 1 : depth)) {
    if (placement.Initialize()) {
      ctrl_->capacity.store(depth == 0 ? 1 : depth, std::memory_order_relaxed);
      ctrl_->head.store(0, std::memory_order_relaxed);
      ctrl_->tail.store(0, std::memory_order_relaxed);
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Producer side. Returns false when full (caller applies the QoS policy).
  bool TryPush(const Record& record) {
    const std::uint64_t capacity =
        ctrl_->capacity.load(std::memory_order_relaxed);
    const std::uint64_t tail = ctrl_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head =
        ctrl_->head.load(std::memory_order_acquire);                // (Q1)
    if (tail - head >= capacity) {
      return false;
    }
    cells_[tail % capacity] = record;               // plain write, see (Q3)
    ctrl_->tail.store(tail + 1, std::memory_order_release);         // (Q3)
    return true;
  }

  // Consumer side. Returns false when empty.
  bool TryPop(Record& out) {
    const std::uint64_t capacity =
        ctrl_->capacity.load(std::memory_order_relaxed);
    const std::uint64_t head = ctrl_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail =
        ctrl_->tail.load(std::memory_order_acquire);                // (Q4)
    if (head == tail) {
      return false;
    }
    out = cells_[head % capacity];                  // plain read, see (Q1)
    ctrl_->head.store(head + 1, std::memory_order_release);         // (Q2)
    return true;
  }

  // Producer-side fullness check (exact from the producer thread: only the
  // consumer can make it less full between this check and TryPush).
  bool Full() const noexcept {
    return ctrl_->tail.load(std::memory_order_relaxed) -
               ctrl_->head.load(std::memory_order_acquire) >=
           ctrl_->capacity.load(std::memory_order_relaxed);
  }

  // Advisory size (for the queue_depth gauge).
  std::size_t Size() const noexcept {
    const std::uint64_t head = ctrl_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = ctrl_->tail.load(std::memory_order_relaxed);
    return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
  }

  std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(
        ctrl_->capacity.load(std::memory_order_relaxed));
  }

 private:
  typename Placement::template SingleHandle<Control> ctrl_;
  typename Placement::template ArrayHandle<Record> cells_;
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

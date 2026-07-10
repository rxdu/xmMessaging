/*
 * detail/latest_slot.hpp
 *
 * LatestSlot<T, Placement, Waiter> — the wait-free depth-1 exchange behind
 * the LatestMailbox contract (design.md), guarantees restated:
 *
 *   1. Store() never blocks and never fails for capacity reasons — the slot
 *      is overwritten (writer is WAIT-FREE: a fixed number of relaxed word
 *      stores plus two fences, no loops, no locks, regardless of readers).
 *   2. Load() returns the newest value or nothing, never a torn value: a
 *      seqlock validates the copy and retries on a concurrent overwrite.
 *      Writer-progress-only: readers retry, the writer NEVER waits — under
 *      a continuously-storing writer a reader is lock-free, not wait-free
 *      (each retry means the writer made progress). This is the documented
 *      bias (design.md POSIX-shm seqlock; R7 grants the writer wait-freedom).
 *   3. Overwritten-unread values are counted — accounting lives one level
 *      up (per-subscriber ordinal-gap accounting in in_process.hpp; see the
 *      note there for why the WRITER cannot count per-reader overwrites
 *      exactly without a lock).
 *   4. Every value is stamped — each cell carries the 48-byte Envelope plus
 *      payload (MailRecord<T>).
 *
 * Data-race freedom (and TSan cleanliness) comes from the Boehm seqlock
 * construction: the record bytes live in an array of relaxed
 * std::atomic<uint64_t> words, ordered by the sequence counter and fences.
 * Memory-ordering pairs, referenced from the code below:
 *
 *   (W1) writer: seq.store(odd, relaxed) THEN atomic_thread_fence(release)
 *        THEN relaxed word stores — any reader whose relaxed word LOAD
 *        reads one of those stores has the writer's release fence
 *        synchronize with the reader's acquire fence (R2), so the reader's
 *        SECOND seq load observes the odd value (or later) and the copy is
 *        rejected. A torn read can never validate.
 *   (W2) writer: final seq.store(even, release) — pairs with the reader's
 *        FIRST seq.load(acquire) (R1): a reader that observes seq == s+2
 *        observes every word store that happened-before it.
 *   (R1) reader: first seq.load(acquire) — see (W2).
 *   (R2) reader: relaxed word loads THEN atomic_thread_fence(acquire) THEN
 *        second seq.load(relaxed) — see (W1).
 *
 * Concurrency contract: SINGLE writer at a time (the exclusive-ownership
 * default, D15). Shared-ownership topics serialize their writers one level
 * up (in_process.hpp) — two unserialized writers would corrupt the sequence.
 * Any number of readers.
 *
 * T must be trivially copyable (word-wise copy). Non-trivially-copyable
 * in-process payloads use MutexLatestSlot below — see its comment for the
 * stated divergence.
 *
 * Placement provides the cell storage (heap now, shared mapping at P1b);
 * Waiter is carried for the parking verbs (never touched by Store/Load) —
 * see placement.hpp / waiter.hpp for the P1b reuse seam.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>

#include "xmmessaging/detail/mail_record.hpp"
#include "xmmessaging/detail/placement.hpp"
#include "xmmessaging/detail/waiter.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// Polite spin hint for seqlock read retries (the only spin in the library,
// and it only spins while the writer is making progress).
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);  // compiler barrier
#endif
}

template <typename T, typename Placement = HeapPlacement,
          typename Waiter = CondvarWaiter>
class LatestSlot {
 public:
  using Record = MailRecord<T>;
  static_assert(std::is_trivially_copyable_v<Record>,
                "LatestSlot requires a trivially copyable record (the "
                "seqlock copies words); non-trivially-copyable payloads use "
                "MutexLatestSlot");

  LatestSlot() : cell_(Placement::template MakeSingle<Cell>()) {
    // Explicit zero-init: an even, zero sequence means "never written".
    cell_->seq.store(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWords; ++i) {
      cell_->words[i].store(0, std::memory_order_relaxed);
    }
  }

  LatestSlot(const LatestSlot&) = delete;
  LatestSlot& operator=(const LatestSlot&) = delete;

  // Writer side. Wait-free: bounded straight-line code, no loops, no locks.
  // Single writer at a time (see the concurrency contract above).
  void Store(const Record& record) noexcept {
    Cell& c = *cell_;
    // Stage the record as words on the stack; the zero fill gives any
    // trailing padding a defined value so the stored words are reproducible.
    std::uint64_t staged[kWords] = {};
    std::memcpy(staged, &record, sizeof(Record));

    const std::uint64_t s = c.seq.load(std::memory_order_relaxed);
    c.seq.store(s + 1, std::memory_order_relaxed);         // odd: in progress
    std::atomic_thread_fence(std::memory_order_release);   // (W1)
    for (std::size_t i = 0; i < kWords; ++i) {
      c.words[i].store(staged[i], std::memory_order_relaxed);
    }
    c.seq.store(s + 2, std::memory_order_release);         // (W2)
  }

  // Reader side. Returns false if the slot was never written. Never blocks
  // the writer; retries while the writer is mid-store (writer progress).
  bool Load(Record& out) const noexcept {
    const Cell& c = *cell_;
    std::uint64_t staged[kWords];
    for (;;) {
      const std::uint64_t s1 = c.seq.load(std::memory_order_acquire);  // (R1)
      if (s1 == 0) {
        return false;  // never written
      }
      if ((s1 & 1u) != 0u) {  // write in progress — retry
        CpuRelax();
        continue;
      }
      for (std::size_t i = 0; i < kWords; ++i) {
        staged[i] = c.words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (c.seq.load(std::memory_order_relaxed) == s1) {
        break;  // copy validated: no overwrite raced it
      }
      CpuRelax();
    }
    std::memcpy(&out, staged, sizeof(Record));
    return true;
  }

  // Parking seam (WaitUntilMatched-class verbs only; NEVER touched by
  // Store/Load — see waiter.hpp).
  Waiter& waiter() noexcept { return waiter_; }

 private:
  static constexpr std::size_t kWords =
      (sizeof(Record) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

  struct Cell {
    std::atomic<std::uint64_t> seq;
    std::atomic<std::uint64_t> words[kWords];
  };

  typename Placement::template SingleHandle<Cell> cell_;
  Waiter waiter_;
};

// Fallback slot for movable-but-not-trivially-copyable in-process payloads
// (the in-process reach accepts any movable C++ type — design.md "Why the
// in-process reach survives the fallback", reason 1).
//
// STATED DIVERGENCE (P0b interim): this slot takes a small mutex on both
// sides, so the R7 wait-free/allocation-free hot-path guarantee holds for
// trivially copyable payloads only — which is also the only class of
// payload for which it CAN hold (copying a std::vector allocates no matter
// what the transport does). Upgrade path if a consumer ever needs a
// lock-free rich-type slot: pointer-rotation triple buffer (P1 candidate).
template <typename T>
class MutexLatestSlot {
 public:
  using Record = MailRecord<T>;

  MutexLatestSlot() = default;
  MutexLatestSlot(const MutexLatestSlot&) = delete;
  MutexLatestSlot& operator=(const MutexLatestSlot&) = delete;

  void Store(const Record& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_ = record;
    written_ = true;
  }

  bool Load(Record& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!written_) {
      return false;
    }
    out = record_;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  Record record_{};
  bool written_ = false;
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

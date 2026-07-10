/*
 * detail/placement.hpp
 *
 * Placement policy: where transport cells live. The slot/ring algorithms
 * (latest_slot.hpp, bounded_queue.hpp) never say `new` — they ask their
 * Placement for storage, sized once at wiring time (R7: all transport
 * memory is sized at wiring from declared QoS; nothing grows afterwards).
 *
 * This is the seam that lets the in-process reach and the P1b POSIX-shm
 * fallback share ONE implementation of each algorithm (design.md "Why the
 * in-process reach survives the fallback"):
 *
 *   - HeapPlacement: process-private heap cells, owning handles, always
 *     freshly zero-initialized (the P0b in-process reach).
 *   - ShmRegionPlacement (P1b): cells are carved out of a pre-sized region
 *     inside a shared mapping (shm_segment.hpp); handles are NON-OWNING
 *     raw pointers (the mapping owns the memory), and zero-initialization
 *     happens only when `initialize` is true — the segment CREATOR (or a
 *     subscriber re-claiming a slot region) initializes; every other
 *     attacher must preserve the contents it finds, because those contents
 *     are live data (the M2/M4 warm-start value survives a publisher
 *     restart precisely because attaching does not re-zero).
 *
 * The algorithms consume a placement INSTANCE (stateless for heap, a
 * region cursor for shm) so the same constructor body serves both; the
 * seqlock/ring code itself never changes — that was the parameterization
 * bet, and P1b is where it pays out.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace xmotion {
namespace messaging {
namespace detail {

// Heap placement: cells are process-private heap memory, allocated at
// wiring time only (never on the publish/take path — R7).
struct HeapPlacement {
  template <typename U>
  using SingleHandle = std::unique_ptr<U>;

  template <typename U>
  using ArrayHandle = std::unique_ptr<U[]>;

  // Fresh heap memory: the algorithm zero-initializes (Initialize() below).
  template <typename U>
  SingleHandle<U> MakeSingle() {
    return std::make_unique<U>();
  }

  template <typename U>
  ArrayHandle<U> MakeArray(std::size_t count) {
    return std::make_unique<U[]>(count);
  }

  // Heap cells are always new — always initialize.
  static constexpr bool Initialize() noexcept { return true; }
};

// Shared-region placement (P1b): a bump allocator over a fixed region of a
// shared mapping. Handles are non-owning raw pointers; the mapping outlives
// them. Determinism contract: every process that attaches a segment
// constructs the SAME algorithm objects in the SAME order over the same
// region, so the bump cursor yields identical offsets everywhere — the
// layout is a function of the payload type and the segment constants,
// never of who attached first.
struct ShmRegionPlacement {
  template <typename U>
  using SingleHandle = U*;

  template <typename U>
  using ArrayHandle = U*;

  ShmRegionPlacement(unsigned char* base, std::size_t size,
                     bool initialize) noexcept
      : base_(base), size_(size), initialize_(initialize) {}

  template <typename U>
  SingleHandle<U> MakeSingle() {
    return Carve<U>(1);
  }

  template <typename U>
  ArrayHandle<U> MakeArray(std::size_t count) {
    return Carve<U>(count);
  }

  bool Initialize() const noexcept { return initialize_; }

  // Bytes consumed so far — used by the layout pass to size regions.
  std::size_t used() const noexcept { return cursor_; }

 private:
  template <typename U>
  U* Carve(std::size_t count) {
    static_assert(std::is_trivially_destructible_v<U>,
                  "shm-placed cells are never destroyed (the mapping is the "
                  "owner); they must be trivially destructible");
    const std::size_t align = alignof(U) < 64 ? 64 : alignof(U);
    cursor_ = (cursor_ + align - 1) & ~(align - 1);
    unsigned char* at = base_ + cursor_;
    cursor_ += sizeof(U) * count;
    assert(cursor_ <= size_ &&
           "xmMessaging: shm region overrun — layout constants disagree");
    if (initialize_) {
      // Value-initialize in place — the same semantics HeapPlacement gets
      // from make_unique's `new U()` (zeroing atomics and PODs), so the
      // algorithms observe identical starting state on both placements.
      for (std::size_t i = 0; i < count; ++i) {
        ::new (static_cast<void*>(at + i * sizeof(U))) U();
      }
    }
    // When attaching (initialize_ == false) the cells were constructed by
    // the initializing process; launder the pointer for strictness.
    return std::launder(reinterpret_cast<U*>(at));
  }

  unsigned char* base_;
  std::size_t size_;
  std::size_t cursor_ = 0;
  bool initialize_;
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

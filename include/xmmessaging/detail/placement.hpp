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
 * in-process reach survives the fallback"): heap placement here; a
 * shared-mapping placement (storage inside a memfd segment) arrives at P1b
 * with the same handle-shaped interface.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <memory>

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

  // Value-initialized so atomics and counters start from zero.
  template <typename U>
  static SingleHandle<U> MakeSingle() {
    return std::make_unique<U>();
  }

  template <typename U>
  static ArrayHandle<U> MakeArray(std::size_t count) {
    return std::make_unique<U[]>(count);
  }
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

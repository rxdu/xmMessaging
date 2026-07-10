/*
 * test/behavioral/alloc_probe.hpp — RAII allocation counter (M1-A4).
 *
 * The telemetry S1 methodology: replace the global allocation functions
 * and count allocations made ON THE PROBING THREAD between AllocProbe
 * construction and query. Include from exactly ONE translation unit per
 * test binary (the replacement operator new/delete definitions below are
 * deliberately non-inline: one definition per program).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <new>

namespace xmmsg_test {

inline thread_local bool g_alloc_counting = false;
inline thread_local std::uint64_t g_alloc_count = 0;

class AllocProbe {
 public:
  AllocProbe() {
    g_alloc_count = 0;
    g_alloc_counting = true;
  }
  ~AllocProbe() { g_alloc_counting = false; }
  AllocProbe(const AllocProbe&) = delete;
  AllocProbe& operator=(const AllocProbe&) = delete;

  std::uint64_t allocations() const { return g_alloc_count; }
};

inline void* CountingAlloc(std::size_t size) {
  if (g_alloc_counting) {
    ++g_alloc_count;
  }
  if (void* p = std::malloc(size != 0 ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}

}  // namespace xmmsg_test

// Replaceable global allocation functions.
void* operator new(std::size_t size) { return xmmsg_test::CountingAlloc(size); }
void* operator new[](std::size_t size) {
  return xmmsg_test::CountingAlloc(size);
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

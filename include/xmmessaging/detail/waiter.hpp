/*
 * detail/waiter.hpp
 *
 * Waiter policy: how a thread parks while it waits for transport-side
 * progress. Used ONLY by bounded parking verbs — Server::WaitForWorkOrShutdown
 * and the reply wait inside Client::Call (D10/D11, P0b part 2) — and NEVER
 * on the publish/take path (R7: those verbs are wait-free / lock-free and do
 * not signal anyone). WaitUntilMatched (D16) deliberately does NOT park here
 * at P0b: see the pthread_cond_clockwait/TSan decision recorded in domain.cpp;
 * any timed wait added on this seam must stay TSan-verifiable on the GCC 11
 * (Ubuntu 22.04, R1) baseline.
 *
 * Which waiter the in-process reach uses (P0b part 2 decision, recorded):
 * the RPC parking verbs use FutexWaiter, NOT CondvarWaiter. Empirically
 * verified on the R1 baseline (GCC 11.4 / Ubuntu 22.04): a timed
 * std::condition_variable wait compiles to pthread_cond_clockwait
 * (CLOCK_MONOTONIC), which that libtsan does NOT intercept — every timed
 * monotonic condvar park produces false "double lock"/data-race reports,
 * and TSan cleanliness is a P0b gate. The futex eventcount below has no
 * such problem: all synchronization is ordinary C++ atomics (which TSan
 * models natively); the futex syscall is only a sleep/wake mechanism with
 * no memory-ordering role, and its relative timeout is judged on
 * CLOCK_MONOTONIC by the kernel — the R8 clock, exactly.
 *
 * This is the second half of the P1b reuse seam (see placement.hpp): the
 * waiter interface (NotifyAll + predicated bounded wait) is what the POSIX
 * shm backend reuses — FutexWaiter already IS the shm-capable shape (the
 * P1b variant moves the epoch word into the shared mapping). CondvarWaiter
 * is retained as the portable non-Linux fallback; it becomes usable for
 * timed parks only when the baseline toolchain's TSan intercepts
 * pthread_cond_clockwait (GCC >= 12).
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "xmbase/types/time.hpp"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#else
#include <thread>
#endif

namespace xmotion {
namespace messaging {
namespace detail {

// Futex-backed eventcount: the in-process (and future POSIX-shm) waiter for
// bounded parking verbs. TSan-verifiable on the R1 baseline (see the header
// comment). Protocol (standard eventcount, lost-wakeup-free):
//   waiter:   gen = epoch.load(acquire); if (pred) done;
//             futex_wait(&epoch, gen, remaining)   // EAGAIN if epoch moved
//   notifier: make pred true; epoch.fetch_add(1, release); futex_wake(all)
// A notifier that bumps epoch after the waiter's load makes the wait return
// immediately (value mismatch), so the pred-recheck can never be lost.
class FutexWaiter {
 public:
  // Wake every parked thread. Called from transition sites (a request
  // becoming pending, a reply landing, a peer detaching) — never from the
  // pub/sub hot path.
  void NotifyAll() noexcept {
    epoch_.fetch_add(1, std::memory_order_release);
    Wake();
  }

  // Park until predicate() is true or deadline_at (monotonic, R8) passes.
  // Returns predicate()'s final value. Spurious wakes and EINTR re-check
  // the predicate and re-park on the remaining time — the bound holds.
  template <typename Predicate>
  bool WaitUntil(::xmotion::Timestamp deadline_at, Predicate predicate) {
    for (;;) {
      const std::uint32_t gen = epoch_.load(std::memory_order_acquire);
      if (predicate()) {
        return true;
      }
      const ::xmotion::Duration remaining = deadline_at - ::xmotion::Now();
      if (remaining <= ::xmotion::Duration::zero()) {
        return predicate();
      }
      WaitOn(gen, remaining);
    }
  }

  // Park for at most max_park (same contract as WaitUntil).
  template <typename Rep, typename Period, typename Predicate>
  bool WaitFor(std::chrono::duration<Rep, Period> max_park,
               Predicate predicate) {
    return WaitUntil(::xmotion::Now() + std::chrono::duration_cast<
                                            ::xmotion::Duration>(max_park),
                     predicate);
  }

 private:
#if defined(__linux__)
  void Wake() noexcept {
    ::syscall(SYS_futex, &epoch_, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr,
              nullptr, 0);
  }

  void WaitOn(std::uint32_t expected, ::xmotion::Duration remaining) noexcept {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)
            .count();
    // FUTEX_WAIT relative timeouts are judged on CLOCK_MONOTONIC (R8).
    timespec ts{static_cast<time_t>(ns / 1000000000),
                static_cast<long>(ns % 1000000000)};
    ::syscall(SYS_futex, &epoch_, FUTEX_WAIT_PRIVATE, expected, &ts, nullptr,
              0);  // EAGAIN/EINTR/ETIMEDOUT all re-enter the caller's loop
  }
#else
  // Portable fallback (non-Linux dev hosts only; the deployed baseline is
  // Linux, R1): bounded sleep-poll, 500 us quantum — the same recorded
  // pattern as WaitUntilMatched (domain.cpp).
  void Wake() noexcept {}

  void WaitOn(std::uint32_t expected, ::xmotion::Duration remaining) noexcept {
    const auto quantum = std::chrono::microseconds(500);
    std::this_thread::sleep_for(
        remaining < ::xmotion::Duration(quantum)
            ? remaining
            : ::xmotion::Duration(quantum));
    (void)expected;
  }
#endif

  // The futex word. 32-bit by kernel contract; the atomic is the ONLY
  // synchronization TSan needs to see (the syscall carries none).
  std::atomic<std::uint32_t> epoch_{0};
  static_assert(sizeof(std::atomic<std::uint32_t>) == 4,
                "futex word must be exactly 32 bits");
};

// Condition-variable waiter (portable reference; NOT used for timed parks on
// the R1 baseline — see the header comment for the TSan evidence).
class CondvarWaiter {
 public:
  // Wake every parked thread. Called from wiring-time paths only.
  void NotifyAll() noexcept {
    {
      // Empty critical section: pairs the notify with a parked thread that
      // is between its predicate check and its wait (the classic lost-wakeup
      // window).
      std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
  }

  // Park for at most max_park or until predicate() becomes true after a
  // notification. Returns predicate()'s final value (condvar semantics).
  template <typename Rep, typename Period, typename Predicate>
  bool WaitFor(std::chrono::duration<Rep, Period> max_park,
               Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, max_park, predicate);
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

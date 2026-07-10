/*
 * detail/waiter.hpp
 *
 * Waiter policy: how a thread parks while it waits for transport-side
 * progress. Used ONLY by bounded parking verbs — Server::WaitForWorkOrShutdown
 * (D10) when the RPC verbs land at P0b part 2 — and NEVER on the
 * publish/take path (R7: those verbs are wait-free / lock-free and do not
 * signal anyone). WaitUntilMatched (D16) deliberately does NOT park here at
 * P0b: see the pthread_cond_clockwait/TSan decision recorded in domain.cpp;
 * any timed wait added on this seam must stay TSan-verifiable on the GCC 11
 * (Ubuntu 22.04, R1) baseline.
 *
 * This is the second half of the P1b reuse seam (see placement.hpp): the
 * in-process reach parks on a condition variable; the POSIX-shm fallback
 * substitutes a futex-based waiter with the same interface, and the slot /
 * ring algorithms are reused unchanged.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace xmotion {
namespace messaging {
namespace detail {

// Condition-variable waiter for the in-process reach.
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

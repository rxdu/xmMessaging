# Lessons

### Timed monotonic condvar waits are not TSan-verifiable on the 22.04 baseline
- **Pattern:** `std::condition_variable::wait_for` / `wait_until` on the steady clock made the D16 barrier fail under ThreadSanitizer with false "double lock of a mutex" reports — libstdc++ implements monotonic condvar waits via `pthread_cond_clockwait`, which GCC 11's libtsan (the Ubuntu 22.04 baseline, R1) does not intercept, so TSan's mutex model is poisoned. Verified with a minimal stand-alone repro before changing library code.
- **Correction:** Where TSan-cleanliness is a gate and the deadline must be monotonic (R8), use a bounded monotonic sleep-poll for wiring-time verbs (see `Domain::WaitUntilMatched`), or restrict timed condvar waits to primitives libtsan intercepts. Re-evaluate at GCC ≥ 12 (interceptor exists there); the `detail::CondvarWaiter` seam is where the event-driven wait returns.
- **Context:** C++17 / libstdc++ / GCC 11 TSan; xmMessaging in-process reach (P0b).

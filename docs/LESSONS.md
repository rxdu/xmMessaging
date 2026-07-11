# Lessons

### Partially replacing global operator new/delete breaks ASan's alloc-dealloc pairing
- **Pattern:** `alloc_probe.hpp` replaced the throwing `operator new`/`delete` (backed by malloc/free) but not the nothrow forms. Under ASan, libstdc++'s `get_temporary_buffer` (used by gtest's `stable_sort` of test cases) allocates via the sanitizer's `operator new(nothrow)` and the buffer is then freed through the probe's free-based `operator delete` — every test in the binary fails at startup with a false `alloc-dealloc-mismatch`.
- **Correction:** When replacing global allocation functions, replace the COMPLETE set (throwing + nothrow, scalar + array, sized deletes) so every allocation path pairs consistently for sanitizer interceptors. Verified: full-set replacement is ASan-clean with no suppression needed.
- **Context:** C++17 / libstdc++ / GCC 11 ASan; xmMessaging behavioral suite (P1b, first ASan leg).

### Timed monotonic condvar waits are not TSan-verifiable on the 22.04 baseline
- **Pattern:** `std::condition_variable::wait_for` / `wait_until` on the steady clock made the D16 barrier fail under ThreadSanitizer with false "double lock of a mutex" reports — libstdc++ implements monotonic condvar waits via `pthread_cond_clockwait`, which GCC 11's libtsan (the Ubuntu 22.04 baseline, R1) does not intercept, so TSan's mutex model is poisoned. Verified with a minimal stand-alone repro before changing library code.
- **Correction:** Where TSan-cleanliness is a gate and the deadline must be monotonic (R8), use a bounded monotonic sleep-poll for wiring-time verbs (see `Domain::WaitUntilMatched`), or restrict timed condvar waits to primitives libtsan intercepts. Re-evaluate at GCC ≥ 12 (interceptor exists there); the `detail::CondvarWaiter` seam is where the event-driven wait returns.
- **Context:** C++17 / libstdc++ / GCC 11 TSan; xmMessaging in-process reach (P0b).

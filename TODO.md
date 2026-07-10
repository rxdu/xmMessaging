# xmMessaging TODO

Phases per docs/design.md; the scenario suite (docs/scenarios.md) gates each one.

## P0.0 — wish-code (the API spec)

- [x] M1 planner→control wish-code (`test/scenarios/m1_planning_control_loop.cpp`) — consumer call-site decided (Sample + tri-state freshness; deltas D1–D5)
- [x] M2 mid-run join wish-code (deltas D6-D7: warm-start intrinsic to latest-only, RAII lifecycle)
- [x] M3 back-pressure wish-code (deltas D8-D9: never-blocking Publish, per-subscriber drops, queryable counters)
- [x] M5 request/response wish-code (deltas D10-D11: threadless take/reply server, non-defaultable deadline)
- [x] M6 reach-parametric fixture wish-code (delta D12: per-backend Domain factories, Supports() matrix query, age_class)
- [x] M7 trace-continuity wish-code (delta D13: auto-capture on publish, explicit RAII adopt on take)
- [x] M13 pipeline-lineage wish-code + M14 stack-startup wish-code (deltas D14-D18)
- [~] Record API deltas in scenarios.md (D1-D18 recorded); freeze P0a header design from them

## P0a — API headers + in-process reach skeleton

- [x] `include/xmmessaging/` API tier — 8 headers, wish-code compiles unmodified as OBJECT lib, zero warnings (deltas D19-D20 record the freeze resolutions)
- [~] schema-hash mechanism (R6): algorithm + vectors specified in wire-contract.md; C++ compile-time generator lands with P0b/P1 matching
- [x] wire-contract spec v0 (docs/wire-contract.md): 64B LE envelope, FNV-1a-64 schema hash + 6 verified conformance vectors, payload rules, metric schema; TBDs marked for P1/P1b/P2
- [ ] per-reach support matrix representation (R3): queryable at wiring time
- [x] xmBase dependency resolution (in-tree > installed > bundled submodule pinned at v0.4.0)
- [ ] M8 lib-only link test (zero transport deps by default)

## P0b — in-process reach behavioral

- [x] LatestMailbox implementation (wait-free depth-1, stamps, overwrite counter) — parameterized over placement (heap vs shared mapping) + waiter (condvar vs futex) from day one, so the POSIX shm backend reuses it unchanged (`detail/latest_slot.hpp`; TSan-clean seqlock)
- [x] queue<N> + reliability policies (`detail/bounded_queue.hpp`: SPSC lock-free; shared-ownership publishers mutex-serialized — lock-free MPMC is a P1 item)
- [~] `messaging.*` self-instrumentation — D9 introspect counters always-on; R11 §7 instruments emitted via the xmBase telemetry API (counters, take_age histogram, gauges); hop_latency histogram + label plumbing + M13-A4 capture verification remain (P0b part 2)
- [ ] M9 in-process benchmark layer (`bench/`: per-verb micro + hop-path; JSON report, CI artifact)
- [~] M1, M2, M3, M5, M6(in-proc), M7(in-proc), M13(in-proc), M14(in-proc) + M9(in-proc) — **v0.1 gate** — M1/M2/M3/M14 behavioral tests pass (plain + TSan, `test/behavioral/`); M5 (Server/Client verbs), M6/M7/M13 tests are P0b part 2
- [ ] R6 schema hash: replace the interim typeid-based description (`detail/schema_hash.hpp`) with the wire-contract §4.2 canonical form + V1–V6 vectors before any hash crosses a process boundary (P1)
- [ ] aarch64 leg of the behavioral suite (R1: seqlock memory ordering must be validated on the weaker memory model in CI)

## P1 — iceoryx2 backend (inter-process)

- [ ] RPC follow-ups from P0b: Qos-declared max-in-flight (fixed 8 now); §7 RPC-endpoint instruments (spec gap); per-instrument labels/units await xmBase metric-API labels; lifecycle signal for WaitForWorkOrShutdown shutdown leg
- [ ] version pin + churn-absorption policy (ADR 0006 open question 1)
- [ ] M1/M2/M3 cross-process, M4 crash recovery, M6 two-process leg
- [ ] M9 backend layer: wrapper-overhead A/B vs raw iceoryx2, budget pinned + CI-gated
- [ ] M10 introspection: shared-memory health segment + CLI tool (list/watch topics, rates, drops, staleness)
- [ ] M11 type-skew refusal (R6): three skew cases + cross-build hash determinism
- [ ] M12 foreign-language participant (R10): Python via native backend binding + spec only — lands with the first backend whose Python binding is mature (Zenoh likely first)
- [ ] binding ladder (R10, on consumer demand): Python -> Go (via C ABI/cgo; no RT guarantees at call site) -> Rust (likely spec-only, native crates)

## P1b — POSIX shm fallback backend (dependency-free inter-process)

- [ ] memfd segment layout + liveness (kernel reclaim, daemonless); writer-progress-only seqlock for latest-only
- [ ] initial support matrix entry: latest-only + best-effort queue; reliable + req/resp declared divergences until implemented
- [ ] M1/M2/M3/M6 cross-process legs runnable without iceoryx2; M4 crash cases against this backend too
- [ ] share the shm substrate with the R5 introspection segment
- [ ] may swap order with P1 if the iceoryx2 pin stalls — this backend has no external dependency to wait on

## P2 — Zenoh backend (inter-host)

- [ ] Zenoh scope decision: network-only vs intra-host fallback (ADR 0006 open question 2)
- [ ] M6 two-host leg (CI: containers/netns); latency envelopes become regression bounds
- [ ] M6-A5 clock semantics (R8): advisory age vs declared synced-clock domain
- [ ] threat model doc (R9): trusted-LAN assumption + revisit trigger — ships with the first Zenoh-backed release

## Meta

- [ ] CI baselines: x86_64 + aarch64 (R1); quickstart time-to-first-message check (R1)
- [ ] Umbrella: `components/messaging` submodule + `XMOTION_WITH_MESSAGING` (default OFF)
- [ ] Migrate wire-vocabulary homes here as they materialize (`ros2_idl` remnants)
- [ ] ADR 0006: propose Proposed → Accepted once the P0b gate passes

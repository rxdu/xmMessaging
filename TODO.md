# xmMessaging TODO

Phases per docs/design.md; the scenario suite (docs/scenarios.md) gates each one.

## P0.0 — wish-code (the API spec)

- [x] M1 planner→control wish-code (`test/scenarios/m1_planning_control_loop.cpp`) — consumer call-site decided (Sample + tri-state freshness; deltas D1–D5)
- [x] M2 mid-run join wish-code (deltas D6-D7: warm-start intrinsic to latest-only, RAII lifecycle)
- [x] M3 back-pressure wish-code (deltas D8-D9: never-blocking Publish, per-subscriber drops, queryable counters)
- [x] M5 request/response wish-code (deltas D10-D11: threadless take/reply server, non-defaultable deadline)
- [x] M6 reach-parametric fixture wish-code (delta D12: per-backend Domain factories, Supports() matrix query, age_class)
- [x] M7 trace-continuity wish-code (delta D13: auto-capture on publish, explicit RAII adopt on take)
- [~] Record API deltas in scenarios.md (D1-D13 recorded); freeze P0a header design from them

## P0a — API headers + in-process reach skeleton

- [ ] `include/xmmessaging/` API tier (Domain, Advertise/Subscribe, QoS vocabulary, statuses)
- [ ] schema-hash mechanism (R6): compile-time layout hash + endpoint match slot — designed before the API freezes; algorithm defined over wire layout, language-neutral, with conformance vectors (R10)
- [ ] wire-contract spec skeleton (R10): envelope byte layout, payload layout rules (standard-layout, explicit padding), topic/QoS conventions — versioned doc under docs/
- [ ] per-reach support matrix representation (R3): queryable at wiring time
- [ ] xmBase dependency resolution (in-tree > installed > bundled submodule, xmTelemetry pattern)
- [ ] M8 lib-only link test (zero transport deps by default)

## P0b — in-process reach behavioral

- [ ] LatestMailbox implementation (wait-free depth-1, stamps, overwrite counter)
- [ ] queue<N> + reliability policies
- [ ] `messaging.*` self-instrumentation
- [ ] M9 in-process benchmark layer (`bench/`: per-verb micro + hop-path; JSON report, CI artifact)
- [ ] M1, M2, M3, M5, M6(in-proc), M7(in-proc) + M9(in-proc) — **v0.1 gate**

## P1 — iceoryx2 backend (inter-process)

- [ ] version pin + churn-absorption policy (ADR 0006 open question 1)
- [ ] M1/M2/M3 cross-process, M4 crash recovery, M6 two-process leg
- [ ] M9 backend layer: wrapper-overhead A/B vs raw iceoryx2, budget pinned + CI-gated
- [ ] M10 introspection: shared-memory health segment + CLI tool (list/watch topics, rates, drops, staleness)
- [ ] M11 type-skew refusal (R6): three skew cases + cross-build hash determinism
- [ ] M12 foreign-language participant (R10): Python via native backend binding + spec only — lands with the first backend whose Python binding is mature (Zenoh likely first)
- [ ] binding ladder (R10, on consumer demand): Python -> Go (via C ABI/cgo; no RT guarantees at call site) -> Rust (likely spec-only, native crates)

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

# xmMessaging TODO

Phases per docs/design.md; the scenario suite (docs/scenarios.md) gates each one.

## P0.0 — wish-code (the API spec)

- [~] M1 planner→control wish-code (`test/scenarios/m1_planning_control_loop.cpp`) — consumer call-site pending (user design input: take shape, staleness surface, degradation hook)
- [ ] M2 mid-run join wish-code
- [ ] M3 back-pressure wish-code
- [ ] M5 request/response wish-code
- [ ] M6 reach-parametric fixture wish-code (the factory that makes M1 run on all reaches)
- [ ] M7 trace-continuity wish-code
- [ ] Record API deltas in scenarios.md; freeze P0a header design from them

## P0a — API headers + in-process reach skeleton

- [ ] `include/xmmessaging/` API tier (Domain, Advertise/Subscribe, QoS vocabulary, statuses)
- [ ] xmBase dependency resolution (in-tree > installed > bundled submodule, xmTelemetry pattern)
- [ ] M8 lib-only link test (zero transport deps by default)

## P0b — in-process reach behavioral

- [ ] LatestMailbox implementation (wait-free depth-1, stamps, overwrite counter)
- [ ] queue<N> + reliability policies
- [ ] `messaging.*` self-instrumentation
- [ ] M1, M2, M3, M5, M6(in-proc), M7(in-proc) as gtest behavioral tests — **v0.1 gate**

## P1 — iceoryx2 backend (inter-process)

- [ ] version pin + churn-absorption policy (ADR 0006 open question 1)
- [ ] M1/M2/M3 cross-process, M4 crash recovery, M6 two-process leg

## P2 — Zenoh backend (inter-host)

- [ ] Zenoh scope decision: network-only vs intra-host fallback (ADR 0006 open question 2)
- [ ] M6 two-host leg (CI: containers/netns); latency envelopes become regression bounds

## Meta

- [ ] Umbrella: `components/messaging` submodule + `XMOTION_WITH_MESSAGING` (default OFF)
- [ ] Migrate wire-vocabulary homes here as they materialize (`ros2_idl` remnants)
- [ ] ADR 0006: propose Proposed → Accepted once the P0b gate passes

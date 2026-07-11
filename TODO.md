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

- [x] `include/xmmsg/` API tier — 8 headers, wish-code compiles unmodified as OBJECT lib, zero warnings (deltas D19-D20 record the freeze resolutions)
- [~] schema-hash mechanism (R6): algorithm + vectors specified in wire-contract.md; C++ compile-time generator lands with P0b/P1 matching
- [x] wire-contract spec v0 (docs/wire-contract.md): 64B LE envelope, FNV-1a-64 schema hash + 6 verified conformance vectors, payload rules, metric schema; TBDs marked for P1/P1b/P2
- [ ] per-reach support matrix representation (R3): queryable at wiring time
- [x] xmBase dependency resolution (in-tree > installed > bundled submodule pinned at v0.4.0)
- [x] M8 lib-only link test (zero transport deps by default) — `test/link/`: gtest-free binary linking only xmmessaging (M8-A1/A2), `ldd` dependency-closure gate (M8-A3), fresh-configure default-OFF check (M8-A1)

## P0b — in-process reach behavioral

- [x] LatestMailbox implementation (wait-free depth-1, stamps, overwrite counter) — parameterized over placement (heap vs shared mapping) + waiter (condvar vs futex) from day one, so the POSIX shm backend reuses it unchanged (`detail/latest_slot.hpp`; TSan-clean seqlock)
- [x] queue<N> + reliability policies (`detail/bounded_queue.hpp`: SPSC lock-free; shared-ownership publishers mutex-serialized — lock-free MPMC is a P1 item)
- [~] `messaging.*` self-instrumentation — D9 introspect counters always-on; R11 §7 instruments emitted via the xmBase telemetry API (counters, take_age histogram, gauges); hop_latency histogram + label plumbing + M13-A4 capture verification remain (P0b part 2)
- [x] M9 in-process benchmark layer (`bench/`: micro/path/system/contended layers, hand-rolled harness per family precedent; `scripts/bench.sh` one-command JSON report with hardware context; M9-A3 alloc gate; report-only comparison vs `bench/reference.json` — M9-A5 values TO BE PINNED by CI; 1 MiB payload deferred to P1 pool sizing)
- [~] M1, M2, M3, M5, M6(in-proc), M7(in-proc), M13(in-proc), M14(in-proc) + M9(in-proc) — **v0.1 gate** — M1/M2/M3/M14 behavioral tests pass (plain + TSan, `test/behavioral/`); M5 (Server/Client verbs), M6/M7/M13 tests are P0b part 2
- [x] R6 schema hash: §4.2 canonical form available via the `XMMSG_DESCRIBE` opt-in (V1–V6 vectors verified, `test/behavioral/wire_schema_test.cpp`; landed at P1b with the first cross-process hash); the interim typeid form remains the fallback for undescribed types — divergence documented in wire-contract §6.4
- [~] aarch64 leg of the behavioral suite (R1: seqlock memory ordering must be validated on the weaker memory model in CI) — `ubuntu-24.04-arm` job added to `.github/workflows/ci.yml`; proven when the first CI run goes green

## P1 — iceoryx2 backend (inter-process)

- [ ] RPC follow-ups from P0b: Qos-declared max-in-flight (fixed 8 now); §7 RPC-endpoint instruments (spec gap); per-instrument labels/units await xmBase metric-API labels; lifecycle signal for WaitForWorkOrShutdown shutdown leg
- [ ] version pin + churn-absorption policy (ADR 0006 open question 1)
- [ ] M1/M2/M3 cross-process, M4 crash recovery, M6 two-process leg
- [ ] M9 backend layer: wrapper-overhead A/B vs raw iceoryx2, budget pinned + CI-gated
- [x] M10 introspection — **landed against the POSIX-shm backend** (P1b introspection follow-up): read-only reader library (`detail/introspect_reader.hpp`), `xmmsg` CLI (`tools/xmmsg`: list/stat/watch/clean, `--json`), `m10_behavioral_test` A1–A5 incl. CLI-subprocess assertions + latency-invisibility A/B; iceoryx2 gains its own introspection story at P1
- [x] M11 type-skew refusal (R6) — **landed against the POSIX-shm backend**: three skew cases + reorder + cross-build determinism via four separately compiled helper builds (`m11_helper_v0..3`), refusal recorded in the segment header (both hashes) and rendered by `xmmsg stat`; re-verify per-backend when iceoryx2 lands
- [ ] M12 foreign-language participant (R10): Python via native backend binding + spec only — lands with the first backend whose Python binding is mature (Zenoh likely first)
- [ ] binding ladder (R10, on consumer demand): Python -> Go (via C ABI/cgo; no RT guarantees at call site) -> Rust (likely spec-only, native crates)

## P1b — POSIX shm fallback backend (dependency-free inter-process)

- [x] segment layout + liveness (`detail/shm_segment.hpp`): `shm_open` named segments — memfd REJECTED (anonymous fds cannot rendezvous by name; decision recorded in wire-contract §6.4 + design.md); pid liveness slots with ESRCH reclaim at wiring paths; ordinals live in the segment and survive publisher crashes; never-unlink lifecycle (CLI cleanup verb deferred to the introspection follow-up)
- [x] writer-progress-only seqlock + SPSC ring reused via `ShmRegionPlacement` — the placement/waiter parameterization bet held: zero algorithm changes (crash-safety additions only: `LatestSlot::LoadBounded` retry budget + `RepairAfterWriterCrash`; `FutexWaiter` gained the shared-word non-PRIVATE form)
- [x] support matrix queryable via `Supports()`, asserted by M6-A6: latest-only, best-effort queue (depth ≤ 16), warm start, deadline = yes; reliable queue, zero-copy loan, req/resp, shared ownership = declared divergences refused at wiring (wire-contract §6.3/§6.4)
- [x] M1/M2/M3/M6 cross-process legs runnable without iceoryx2 (fork+exec `shm_test_helper`) + M4 crash cases (SIGKILL mid-stream: no torn/blocked reads, staleness rises; restart re-advertises + rejoins; MatchedCount observes death/rejoin; ordinal continuity across restart) — plain + TSan + ASan suites green (TSan cross-process blindness documented in `shm_test_support.hpp`)
- [x] R6 canonical schema hash for cross-process matching: `XMMSG_DESCRIBE` opt-in generates the wire-contract §4.2 description at wiring time, §5 vectors V1–V6 verified (`wire_schema_test`); undescribed types keep the interim typeid hash — documented divergence (§6.4): not foreign-computable, misses the M11-A2 reorder
- [x] share the shm substrate with the R5 introspection surface — resolved as designed: no separate segment, the per-topic header (layout v2: + refusal-visibility record) IS the surface; discovery = /dev/shm scan + §6.4 grammar; read protocol + header byte layout normative in wire-contract §8; `xmmsg` CLI ships (`XMMESSAGING_BUILD_TOOLS`, default ON); ClockDomain encoding deferred to P2 (same-host backend has one clock — §8.5)
- [ ] align the segment's per-record bytes with the §2 64-byte envelope frame before M12 targets this backend (recorded divergence, wire-contract §6.4)
- [ ] reliable queue over shm (cross-ring all-or-nothing pre-check) — implement or declare permanent at P1
- [ ] distinct wiring status for "contract unsupported on this reach" vs "backend not built" (both read kUnsupportedReach today) — P1 API decision
- [x] may swap order with P1 if the iceoryx2 pin stalls — it did: P1b landed first (no external dependency to wait on)

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

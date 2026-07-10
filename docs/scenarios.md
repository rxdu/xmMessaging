# xmMessaging Scenario Suite — the use cases that define the API

- Status: **P0.0 wish-code in progress** (no scenario is executable yet; the API does not exist)
- Date: 2026-07-10
- Governing design: [design.md](design.md) and [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md)

## Why scenarios come first

Same method that shipped the telemetry stack: rather than freezing an API with zero consumers, this suite writes the consumers first — realistic composition use cases, expressed as the wiring code we *wish* an application could write. The API is then shaped to serve them. Each scenario lives three lives:

1. **Wish-code (P0.0)** — skeleton call sites in `test/scenarios/` that define the ideal API. Not compiled; they *are* the API spec. Gaps they surface are recorded in [API deltas](#api-deltas-discovered-by-the-wish-code) below.
2. **Behavioral test (P0b+)** — compiled against the real API; acceptance criteria asserted with gtest.
3. **Benchmark / regression (P1+)** — M1 and M3 become permanent latency/back-pressure gates run in CI on every change.

**Gate for the v0.1 (in-process reach) release**: M1, M2, M3, M6, M8 pass as behavioral tests on the in-process reach. M4 requires iceoryx2 (P1); M7's cross-process half requires a real IPC backend; M5 lands with the first backend that carries it.

## Conventions established here

- **Fakes, not components**: scenarios use synthetic producers/consumers (fake planner emitting trajectory heads, fake control loop, fake config server) so the suite has zero dependency on xmNavigation/xmDriver. Payload structs are scenario-local PODs shaped like the real vocabulary.
- **Topic names**: lowercase dot-separated `<domain>.<subsystem>.<name>`; scenario topics are prefixed `m<N>.` to stay out of real namespaces.
- **Endpoints are acquired at wiring time, used on the hot path** — never name-lookup inside a loop (the telemetry handle rule, applied to messaging).
- **Every acceptance criterion that involves loss or delay must be observable** — asserted through the library's own `messaging.*` telemetry instruments where possible, so the scenarios also prove the self-instrumentation story.
- **Reach parity**: any scenario marked *reach-parametric* must pass with identical assertion code on every reach it runs on; only the wiring fixture changes.

## Catalog

| # | Scenario | File | Contract proven | Reach | Executable at |
|---|----------|------|-----------------|-------|---------------|
| M1 | Planner → control at rate (10 Hz plans, 1 kHz consumer) | `test/scenarios/m1_planning_control_loop.cpp` | LatestMailbox depth-1, stamps, deadline | reach-parametric | P0b (in-proc), P1 (iceoryx2) |
| M2 | Mid-run subscriber join | `test/scenarios/m2_late_join.cpp` | latest-only warm-start for late joiners | reach-parametric | P0b, P1 |
| M3 | Slow consumer / back-pressure / flood | `test/scenarios/m3_backpressure.cpp` | explicit publish status, counted drops | reach-parametric | P0b, P1 |
| M4 | Crash of one process, recovery | `test/scenarios/m4_crash_recovery.cpp` | daemonless survival, staleness detection, rejoin | inter-process | P1 |
| M5 | Request/response with deadline | `test/scenarios/m5_request_response.cpp` | typed RPC, absent-server & timeout statuses | reach-parametric | P0b (in-proc), P1+ |
| M6 | Reach transparency | `test/scenarios/m6_reach_transparency.cpp` | same composition code across thread/process/host | all three | P0b → P2 (grows with backends) |
| M7 | Trace continuity across the hop | `test/scenarios/m7_trace_continuity.cpp` | envelope carries telemetry context | reach-parametric | P0b, P1 |
| M8 | Lib-only build / backend optionality | `test/scenarios/m8_libonly_build.cpp` + link test | in-proc reach with zero transport deps | in-process | P0a |

---

## M1 — Planner → control at rate

**Purpose.** The flagship coupling this library exists for (designed 2026-07-06): a planner produces trajectory heads at ~10 Hz; a control loop at 1 kHz consumes the **latest** plan every cycle and must know how stale it is. This scenario forces the LatestMailbox contract, the stamp/deadline surface, and the hot-path discipline.

**Setup.** Fake planner thread/process publishes a `TrajectoryHead` POD (a few hundred bytes) at 10 Hz on `m1.plan.head` with `history = latest-only`. Fake control loop runs at 1 kHz, takes the latest head each cycle, and computes a setpoint from it. Deadline: 250 ms (2.5 planning periods).

**Acceptance criteria.**
- A1: the control loop observes every published plan at least once **or** the overwrite counter accounts for it — no silent loss, no phantom values.
- A2: values are never torn: a marker field written first and a checksum written last always agree on the consumer side, at full rate, under ThreadSanitizer (in-proc reach).
- A3: consumer-side staleness is queryable per-take and correct within clock resolution; when the planner is paused, the deadline flag raises within one control cycle of the bound.
- A4: zero heap allocations and zero blocking on both the publish and the take path once wired (in-proc reach; alloc-probe methodology from telemetry S1).
- A5: the library's own instruments (`messaging.*` publish count, overwrite count, take-age histogram) reconcile with the scenario's ground truth.

## M2 — Mid-run subscriber join

**Purpose.** Robots attach tools mid-run (a tuner, a recorder, a dashboard). A subscriber that joins late on a latest-only topic must warm-start from the current value, not stare at emptiness until the next publish.

**Setup.** Publisher on `m2.robot.state` (latest-only, 1 Hz — deliberately slow). A second subscriber is created 500 ms after a publish.

**Acceptance criteria.**
- A1: the late joiner's first take yields the current value (with its original stamp) without waiting for the next publish, on every reach that can support it; where a backend cannot, the limitation is a **stated, tested** difference, not a surprise.
- A2: joining and leaving does not perturb the existing subscriber (no missed or duplicated values on the incumbent).
- A3: N subscribers on one topic each independently hold the LatestMailbox contract.

## M3 — Slow consumer, back-pressure, flood

**Purpose.** The Aeron lesson as a test: capacity exhaustion must be explicit at the publish site and counted everywhere. Also proves the flood-isolation property: one misbehaving topic must not degrade its neighbors.

**Setup.** Topic A: `queue<8>`, `reliable`, consumer artificially stalled. Topic B: `queue<8>`, `best-effort`, same stall. Topic C: healthy latest-only control traffic sharing the same context. Publisher floods A and B.

**Acceptance criteria.**
- A1: on A, `Publish` returns would-block once the queue is full — it never blocks the caller silently and never drops silently; total published = delivered + explicitly-refused.
- A2: on B, overflow drops occur, and delivered + counted-drops = published, exactly.
- A3: topic C's latency and loss are statistically indistinguishable from an unflooded run (isolation).
- A4: drop and refusal counters are visible as `messaging.*` telemetry without any scenario-side plumbing.

## M4 — Crash of one process, recovery

**Purpose.** The daemonless argument made falsifiable: with no broker to die, one participant's death must be a local event. This is the scenario that justified choosing iceoryx2 over RouDi-era iceoryx and Aeron.

**Setup.** Planner process and control process coupled as in M1, inter-process reach. Planner is SIGKILLed mid-stream; restarted after 2 s.

**Acceptance criteria.**
- A1: the control process never blocks, crashes, or receives a torn value across the kill — it observes rising staleness and its deadline flag, nothing else.
- A2: transport resources owned by the dead process are reclaimed such that the restarted planner can re-advertise the same topic and publish successfully.
- A3: the control process receives fresh values after rejoin with no re-wiring code — the subscription outlives the peer.
- A4: the death and rejoin are observable events (telemetry), not just inferable from staleness.

## M5 — Request/response with deadline

**Purpose.** The query pattern: a controller asks a parameter server for gains at mode-switch time. Proves the typed RPC surface and its refusal to wait forever.

**Setup.** Fake parameter server exposes `m5.config.get_gains` (`Client<GainsRequest, GainsResponse>`). Cases: server present and prompt; server present but slower than the deadline; server absent.

**Acceptance criteria.**
- A1: the prompt case returns the typed response, with the request's telemetry context propagated through the server's handler span.
- A2: the slow case returns deadline-expired at the deadline (±10%), and the late response is discarded without leaking or arriving on a later call.
- A3: the absent case fails fast with a distinct no-server status — distinguishable from timeout.
- A4: a call with no deadline supplied does not compile (or is impossible to express) — the API makes unbounded waits unrepresentable.

## M6 — Reach transparency

**Purpose.** The library's central promise, tested literally: composition code written once runs on all three reaches.

**Setup.** The M1 planner/control pair factored into a fixture where publisher and subscriber wiring comes from a reach-parameterized factory. Three instantiations: same-thread pool, two processes (iceoryx2), two hosts (Zenoh; two containers/netns in CI).

**Acceptance criteria.**
- A1: scenario assertion code is byte-identical across the three instantiations; only the fixture differs.
- A2: the LatestMailbox contract (M1 A1–A3) holds on every reach.
- A3: measured hop latency fits each reach's published envelope (recorded, not gated at first; becomes a regression bound at P2).
- A4: a payload type lacking what a reach requires (e.g. not trivially copyable on the zero-copy path, no serializer for network) is a **compile-time** error at wiring, not a runtime failure.

## M7 — Trace continuity across the hop

**Purpose.** The envelope contract as a test: the "planning stall and motor fault on one timeline" story must survive the transport hop with zero per-application discipline.

**Setup.** Producer publishes under an active trace (`NewTrace` + span); consumer adopts the extracted context and opens its own span. Run on in-proc and iceoryx2 reaches.

**Acceptance criteria.**
- A1: producer and consumer spans share one `TraceId` with correct causal linkage, across the process boundary, with no context-handling code beyond publish/take.
- A2: two interleaved traces through one topic do not cross-contaminate (the telemetry S2-A4 property, across the transport).
- A3: envelope overhead per message is fixed-size and accounted for in the loan/payload budget.

## M8 — Lib-only build / backend optionality

**Purpose.** Honest optionality, the telemetry S6 of this library: an application composing in one process links xmMessaging and gets the full in-process reach with **zero** transport dependencies; backends are strictly opt-in.

**Setup.** Link test builds an in-proc M1 against xmMessaging configured with all backend options OFF; a CI check asserts the resulting binary's dependency closure contains no iceoryx2/Zenoh symbols or shared-library references.

**Acceptance criteria.**
- A1: default configuration (`XMMESSAGING_WITH_ICEORYX2=OFF`, `XMMESSAGING_WITH_ZENOH=OFF`) builds and passes in-proc scenarios with xmBase as the only family dependency.
- A2: requesting an inter-process/inter-host reach at wiring time in a lib-only build fails at compile time (preferred) or with an explicit unsupported-reach status — never a silent in-proc fallback: a robot that silently isn't distributed is a field incident.
- A3: each backend option adds only its own dependency (verified by dependency-closure diff in CI).

---

## API deltas discovered by the wish-code

Recorded as the wish-code is written; each delta feeds the P0a API design. (None yet — P0.0 starts now.)

| # | Discovered in | Delta |
|---|---------------|-------|

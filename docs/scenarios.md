# xmMessaging Scenario Suite — the use cases that define the API

- Status: **P0.0 wish-code complete** (all 14 scenarios wish-coded, deltas D1–D18 recorded; nothing executable yet — the API headers are the next phase, P0a, derived from the deltas)
- Date: 2026-07-10
- Governing design: [design.md](design.md) and [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md)

## Why scenarios come first

Same method that shipped the telemetry stack: rather than freezing an API with zero consumers, this suite writes the consumers first — realistic composition use cases, expressed as the wiring code we *wish* an application could write. The API is then shaped to serve them. Each scenario lives three lives:

1. **Wish-code (P0.0)** — skeleton call sites in `test/scenarios/` that define the ideal API. Not compiled; they *are* the API spec. Gaps they surface are recorded in [API deltas](#api-deltas-discovered-by-the-wish-code) below.
2. **Behavioral test (P0b+)** — compiled against the real API; acceptance criteria asserted with gtest.
3. **Benchmark / regression (P1+)** — M1 and M3 become permanent latency/back-pressure gates run in CI on every change.

**Gate for the v0.1 (in-process reach) release**: M1, M2, M3, M6, M8, and the in-process legs of M13 and M14 pass as behavioral tests, plus M9's in-process benchmark layer (a release without its reference numbers violates R4). M4 and M10 require an IPC backend (P1/P1b); M7's cross-process half likewise; M5 lands with the first backend that carries it.

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
| M8 | Lib-only build / backend optionality | `test/scenarios/m8_libonly_build.cpp` + link test | in-proc reach with zero transport deps (R1) | in-process | P0a |
| M9 | Benchmark suite & wrapper-overhead budget | `bench/` | thin wrapper, measured (R3, R4) | all three | P0b (in-proc), grows with backends |
| M10 | External introspection | `test/scenarios/m10_introspection.cpp` + CLI | outside-the-process diagnostics (R5) | inter-process | P1 |
| M11 | Rebuild skew: type-identity refusal | `test/scenarios/m11_type_skew.cpp` | schema-hash match gate (R6) | inter-process | P1 |
| M12 | Foreign-language participant | `test/scenarios/m12_foreign_participant/` | wire contracts, not bindings (R10) | inter-process / inter-host | P1–P2 (first backend with a mature Python binding) |
| M13 | Pipeline lineage & system evaluation | `test/scenarios/m13_pipeline_accounting.cpp` | origin age, metric schema, one timeline (R11) | reach-parametric | P0b, P1 |
| M14 | Stack cold start: order, readiness, isolation, ownership | `test/scenarios/m14_stack_startup.cpp` | composition-scale contracts | reach-parametric | P0b, P1 |

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
- A5 *(R8)*: on the two-host leg without a declared synchronized-clock domain, taken values expose age as *advisory* (distinctly typed/flagged from same-host age) and no deadline verdict is issued; with the domain declared, deadline semantics apply and the declaration is visible in introspection.
- A6 *(R3 divergence-over-emulation)*: any contract a reach does not natively support is reported at wiring time via the support matrix — the scenario asserts the query result matches the documented matrix, and no silent emulation kicks in.

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

## M9 — Benchmark suite & wrapper-overhead budget

**Purpose.** R3 and R4 made falsifiable: "thin" and "fast" are measured claims, and a user must be able to reproduce the measurement on their own hardware with one command.

**Setup.** `bench/` tier (google-benchmark), three layers. *Micro*: per-verb cost — `Publish`, `Loan`+`Publish`, take — on the in-process reach, and the same verbs through each backend **paired with the identical operation on the raw backend API**, so wrapper overhead is a direct A/B. *Path*: end-to-end hop latency percentiles (p50/p99/p99.9/max), throughput, and jitter per reach, swept across payload sizes ({64 B, 1 KiB, 64 KiB, 1 MiB}) and the M1 rate profile. *System (R4)*: the robot-typical profile — ~30 topics at mixed 10 Hz–1 kHz rates, small payloads, concurrent — because single-topic sweeps flatter every transport.

**Acceptance criteria.**
- A1: one command (`ctest` label or `scripts/bench.sh`) runs the full suite and emits a machine-readable report (JSON) plus a human summary; the report embeds the hardware context (CPU model, governor, kernel + RT patch, isolation, load state) — a context-less report is a failing run.
- A2: wrapper overhead per verb vs the raw backend stays within a pinned budget (target set at P1 calibration; the point is the *gate exists*, per-verb, in CI).
- A3: in-process publish and take are allocation-free and lock-free, proven by probe, at every payload size in the sweep.
- A4: results are published as CI artifacts per release, and the README table of reference numbers is generated from them — never hand-written.
- A5: a regression against the pinned reference (beyond noise bounds) fails CI, same discipline as the telemetry S1 overhead gate.
- A6 *(R7 — WCET is established under contention, not assumed from idle runs)*: every micro-benchmark has a contended variant — a writer racing N readers on one slot, the flood profile running concurrently — and the R7 tail gates apply to the contended numbers.

## M10 — External introspection

**Purpose.** R5 as a test: when "the controller isn't getting plans" happens on a robot, diagnosis must not require rebuilding, instrumenting, or even cooperating application code.

**Setup.** The M1 pair runs as two unmodified processes (inter-process reach). A third process — the CLI tool, plus a raw reader in the test — attaches to the introspection segment. Fault injections: planner paused (staleness), consumer stalled (queue growth/drops), planner killed (endpoint death).

**Acceptance criteria.**
- A1: the external process enumerates live topics and endpoints with types, QoS, and owning PIDs — with zero cooperation from the observed processes.
- A2: per-topic rate, queue depth, drop/overwrite counters, and last-publish age read externally reconcile with the application's own `messaging.*` telemetry.
- A3: each injected fault is diagnosable from the CLI output alone (staleness rises; drops count up; the dead endpoint is marked, not vanished silently).
- A4: attaching/detaching the observer is invisible to the observed processes' latency profile (measured against an M9 baseline run).
- A5: reading the segment is safe against a concurrently crashing publisher — the observer never blocks, crashes, or reads torn counters.

## M11 — Rebuild skew: type-identity refusal

**Purpose.** R6 as a test: the field incident where two processes built from different commits disagree about a payload's layout must be impossible to hit silently.

**Setup.** Two builds of the M1 planner/control pair from deliberately divergent payload definitions, three skew cases: a field appended (size change), two fields reordered (same size, different layout — the nasty one), a field's type changed at same offset/size. Also the control case: identical layouts built separately.

**Acceptance criteria.**
- A1: in all three skew cases, the endpoint match is refused with the distinct type-mismatch status — the subscriber never receives a single reinterpreted byte.
- A2: the reorder case (identical `sizeof`, identical type name) is caught — proving the hash covers layout, not just name and size.
- A3: the refusal is visible in introspection (M10 tooling): both endpoints listed, marked incompatible, with both schema hashes shown.
- A4: the control case matches — separately-built identical layouts produce identical hashes (hash is deterministic across builds, not a build fingerprint).
- A5: the refusal is local — other topics between the same two processes keep flowing.

## M12 — Foreign-language participant

**Purpose.** R10 as a test: a component outside the family, in another language, joins the system using only its backend's native binding plus the published wire-contract spec — zero xmMessaging code on its side.

**Setup.** The C++ M1 pair runs unmodified. A Python process, written against the backend's native Python binding and the wire-contract spec (no generated code from this repo), (a) subscribes to `m1.plan.head` and (b) publishes a valid payload back on a second topic consumed by the C++ side. The spec artifacts under test: envelope layout, schema-hash algorithm, topic/QoS mapping, payload layout rules.

**Acceptance criteria.**
- A1: the Python subscriber decodes payloads and envelope (stamp + telemetry context) correctly, verified against the C++ side's ground truth — byte-for-byte on the envelope fields.
- A2: the Python side computes the R6 schema hash **from the spec's conformance vectors alone** and matches the C++-computed hash for the same layout; a deliberately wrong layout is refused at match, proving the gate holds for foreign participants too.
- A3: the C++ consumer cannot distinguish the Python publisher from a C++ one: same contracts, same staleness surface, same introspection visibility (the Python endpoints appear in M10 tooling).
- A4: the spec is sufficient — the scenario's Python code imports nothing from this repository, and CI runs it from the spec + backend binding only. Any needed clarification is a spec defect, recorded like an API delta.
- A5: a payload violating the cross-language layout rules (implicit padding) is rejected at the C++ wiring site at compile time — the rule exists on the family side, not just in prose.

## M13 — Pipeline lineage & system evaluation

**Purpose.** R11 as a test: a three-stage fake navigation pipeline (sensor → estimator → controller) whose end-to-end behavior is quantitatively evaluable from what the transport records — zero custom instrumentation.

**Setup.** Sensor fake publishes poses at 100 Hz; estimator consumes, publishes state estimates via `PublishDerived` (lineage preserved); controller consumes at 1 kHz. Telemetry SDK bound with a capture sink. Fault injections: estimator stalled 300 ms (information ages while plans stay fresh); estimator restarted.

**Acceptance criteria.**
- A1: at the controller, `sample.origin_age()` reports the sensor-acquisition age and `sample.age()` the last-hop age; under the estimator stall, origin_age grows while age stays small — the two are provably distinct at the call site.
- A2: hop count increments per stage; a first-hop publish has origin == stamp and hops == 0.
- A3: end-to-end (sensor→controller) latency distribution is computable **from the captured telemetry records alone** (standard metric schema + envelope fields), and matches ground truth within clock resolution.
- A4: every standard-schema instrument for all six endpoints is present in the capture with correct units and labels — no endpoint opted anything in.
- A5: messaging stamps and telemetry record timestamps interleave correctly on one timeline (a transport hop never appears to precede the publish span that caused it).

## M14 — Stack cold start: order, readiness, isolation, ownership

**Purpose.** The composition-scale contracts as tests: arbitrary process start order, a bounded readiness barrier, domain isolation on a shared host, and the exclusive-ownership default.

**Setup.** The M13 three-stage pipeline, started in all permutations of stage order (in-proc: thread start order; inter-process: process spawn order). Second copies: a duplicate estimator (ownership case) and a full second stack instance under a different domain key (isolation case).

**Acceptance criteria.**
- A1: every start permutation converges to the same wired state — `Subscribe` before `Advertise` is indistinguishable, after matching, from the reverse order.
- A2: `WaitUntilMatched(endpoints, deadline)` returns success exactly when all listed endpoints have peers, and a distinct timeout status when the deadline passes with the estimator deliberately absent — the launcher's e-stop-release gate is buildable from this one verb.
- A3: the duplicate estimator's `Advertise` on the exclusive topic is refused with the ownership status; after declaring `Ownership::kShared` on both, latest-only resolves last-writer-wins by stamp, deterministically under interleaving.
- A4: the second stack instance under a different domain key shares nothing — no topic visibility, no shm segments, no introspection entries — verified positively (its own domain works) and negatively (cross-domain take never yields the other stack's data).
- A5: `MatchedCount()` on each endpoint tracks join/leave of peers correctly through the estimator restart.

---

## API deltas discovered by the wish-code

Recorded as the wish-code is written; each delta feeds the P0a API design.

| # | Discovered in | Delta |
|---|---------------|-------|
| D1 | M1 consumer | `TakeLatest()` returns `Sample<T>` (value + monotonic stamp + freshness verdict) — never an optional. Emptiness is a freshness state, not a different return type, so the call-site can't skip handling it. |
| D2 | M1 consumer | `Freshness` is tri-state: `kFresh` / `kStale` (exists, past the wiring-time deadline — value still accessible) / `kNone` (never received). The library judges it against the declared deadline; callers can still read `age()` for custom logic. Dereferencing a `kNone` sample is a contract violation (debug-assert). |
| D3 | M1 consumer | One deadline declaration, two surfaces: the per-take verdict at the call-site and a `messaging.*` deadline-miss event on the Fresh→Stale transition. They must agree by construction (same config, same clock). |
| D4 | M1 consumer | Consumer-side zero-copy `View()` (variant B) **declined for now**: no current payload justifies imposing a hold-time contract on the hottest loop, and the POD copy is measurable in M9 if that ever changes. Revisit when a payload size demands it. |
| D5 | M1 consumer | Bounded-wait `WaitFor()` (variant C) excluded from the paced-loop pattern — the loop owns its cadence, never the transport. May return later as a separate verb for event-driven (non-loop) consumers; that consumer would be its own scenario. |
| D6 | M2 | Warm-start is **intrinsic to latest-only**, not a fifth QoS knob (no DDS-style durability axis): the slot exists, a new subscriber reads it. The warm-start value keeps its **original stamp** — age must never report ~0 for old data (R8). |
| D7 | M2 | Subscription lifecycle is undramatic: mid-run join is the same `Subscribe` call as wiring-time; teardown is scope exit (RAII), no unsubscribe ceremony; each subscriber owns an independent mailbox (one's take never disturbs another's). |
| D8 | M3 | `Publish` **never blocks internally**. `kWouldBlock` occurs only on reliable+bounded-full and means nothing was enqueued; retry/coalesce/shed is caller policy. Best-effort `Publish` always returns `kOk` — overflow drops are **per-subscriber** facts in the subscriber's counters, because delivered-to-whom is an endpoint-local truth, not a publisher status. |
| D9 | M3 | Endpoint counters (drops, refusals, overwrites) are programmatically queryable (`msg::introspect::*`) in addition to telemetry and the CLI — scenarios and applications can reconcile conservation exactly, in code. |
| D10 | M5 | The server owns no threads (R3): `Serve<Req,Rsp>` yields a take/reply endpoint polled from an app-owned loop; `TakeRequest()` reuses the D1/D2 Sample surface, and the taken request carries the reply token + caller's telemetry context. Needs a bounded-park verb (`WaitForWorkOrShutdown`) so a server loop neither busy-spins nor sleeps blind. |
| D11 | M5 | `Call(req, deadline)` has no deadline-less overload — unbounded waits are unrepresentable (M5-A4). Returns `Result<T>` (status + value, mirroring `Sample<T>`); `kDeadlineExpired` and `kNoServer` are distinct; late replies are discarded by correlation, never surfacing on later calls. Async `Call` variant deferred — no consumer shape demands it yet. |
| D13 | M7 | Context propagation is **asymmetric by design**: `Publish` captures the calling thread's active telemetry context automatically (null context if none — fixed envelope size either way); the take side adopts **explicitly** via a `tel::ContextScope` RAII over `sample.context()`, restoring the prior thread context at scope exit. Automatic adoption is rejected — it would silently reparent a consumer's own trace (the cross-contamination M7-A2 forbids). The context travels with the value: an overwritten latest-only value takes its context with it. |
| D14 | M13 | The envelope gains **lineage**: origin stamp + hop count, fixed-size. `PublishDerived(loan, upstream_sample)` preserves origin (oldest consumed input) and increments hops; plain `Publish` is hop zero. `Sample::origin_age()` sits beside `age()`. Per-hop decomposition stays out of the envelope — reconstructed offline from M7 trace links. |
| D15 | M14 | Fifth QoS knob **Ownership**: `kExclusive` default (second `Advertise` refused, distinct status) / `kShared` by declaration (latest-only = last-writer-wins by publish stamp). |
| D16 | M14 | Readiness surface: `MatchedCount()` per endpoint + `Domain::WaitUntilMatched(endpoints, deadline)` as the single bounded barrier verb. No match-event callbacks — that would reintroduce hidden execution (R3). |
| D17 | M14 | Every `Domain` factory takes an **isolation key** (default derived from user + configured name); topics, segments, and introspection are namespaced by it. Two stacks on one host share nothing implicitly. |
| D18 | M14 | Endpoint construction does not throw: `Advertise`/`Subscribe` return handles carrying a queryable status (`kOk`, `kOwnershipRefused`, type-mismatch per R6, unsupported-reach per M8-A2), so a launcher can enumerate exactly what failed. Consuming an invalid handle is a contract violation (debug-assert), mirroring the D2 `kNone`-dereference rule. |
| D12 | M6 | `Domain` factories are per-backend with backend-specific config structs (the portable API never grows a union of engine options). The R8 clock declaration (`ClockDomain`) lives in the backend config and is introspection-visible. `domain.Supports(Contract::k…)` queries the R3 support matrix at wiring time. `Sample::age_class()` is `kMeasured`/`kAdvisory` — an advisory age **never** yields a `kStale` verdict; unsynced clocks must not produce confidently-wrong staleness. |

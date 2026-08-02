# xmMessaging Library Design

- Status: **Draft** (P0.0 — scenario suite is being written; this document records the contracts the scenarios must prove)
- Date: 2026-07-10
- Governing decision: [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md) (xmMessaging: the application-level communication layer)
- Related: [ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md) (backend-seam pattern), [ADR 0005](https://github.com/rxdu/xmotion/blob/main/docs/adr/0005-application-level-composition.md) (applications own composition)

## What this library is

xmMessaging (`xmotion::messaging`) moves typed data between the components an application composes. It is the glue tier of ADR 0005 made concrete: algorithm components (xmNavigation) and hardware components (xmDriver) keep producing and consuming plain xmBase types and **never link this library**; applications link it to wire those components into a running system.

## Requirements

Stated explicitly (2026-07-10; refined same day after design review) so every design choice below and every scenario in [scenarios.md](scenarios.md) traces to one:

- **R1 — Lightweight and portable.** Trivial to set up: xmBase is the only mandatory dependency, plain CMake ≥ 3.16, Ubuntu 22.04/24.04 on **x86_64 and aarch64** as the tested baselines — robots ship on ARM, and its weaker memory model is where "works on x86" lock-free code breaks, so aarch64 is a CI target, not a porting afterthought. Each backend is acquired by one declared mechanism (pinned submodule or system package) behind its option — enabling a backend must never become a toolchain project. Measurable claim: **time-to-first-message under 10 minutes** from a clean machine, quickstart tested in CI. Traced by M8; deb packaging follows the family pattern.
- **R2 — A clear, small API.** The surface stays countable: `Domain`, `Advertise`/`Subscribe`, `Publish`/`Loan`/take, `Client`/`Server`, the five QoS knobs, and the status enums — nothing else. Testable form: every public verb appears in at least one scenario, the cookbook covers every knob, and **every failure mode a user can hit has a distinct, documented status** — APIs are confusing at the error surface, not the happy path. Wish-code *is* the usability test: if it reads awkwardly, the API is wrong; deltas get recorded, not tolerated.
- **R3 — A thin wrapper.** Conventions and type safety over the engines, not a new middleware: no hidden threads, no hidden allocation, no policy magic the user didn't select. Three concrete commitments: (a) a **native escape hatch** — `Native()` on endpoints exposes the underlying backend handle, explicitly non-portable by declaration; (b) a **measured overhead budget** — wrapper cost over the raw backend is benchmarked per verb and regression-gated, not asserted; (c) **divergence over emulation** — when a backend cannot natively honor a portable contract, the library documents and tests the divergence in a per-reach support matrix (queryable at wiring time) rather than building emulation layers. Traced by M9, M6.
- **R4 — First-class performance benchmarks.** One command, on the user's own hardware, yields latency (**p50/p99/p99.9/max** — tails, not means), throughput, and jitter per reach, comparable against published reference numbers and against the raw backend. Every report captures its **hardware context** (CPU model, governor, kernel + RT patch, isolation, load state) — numbers without context are noise. The suite includes a **robot-typical profile** (tens of topics, mixed 10 Hz–1 kHz rates, small payloads) alongside single-topic sweeps, because single-topic p50 is where middleware benchmarks traditionally lie. In-tree, CI-run, results are release artifacts. Traced by M9.
- **R5 — Runtime introspection.** Diagnostics must not require rebuilding or cooperating application code: transport health (topics, endpoints, rates, queue depths, drops, staleness) is observable from *outside* the process at runtime — shared-memory counters (the Aeron idea adopted in ADR 0006) plus a CLI tool. Scope boundary: the CLI shows **live state only**; anything historical is the telemetry plane's job, offline, through existing viewers. Traced by M10.
- **R6 — Type identity across the wire.** Endpoint matching verifies payload type identity with a compile-time-derived **schema hash** (fields, offsets, sizes) carried by every endpoint; a mismatch refuses the match with an explicit status and is visible in introspection. Two processes built from different commits must find it impossible to exchange silently-reinterpreted bytes — rebuild skew is a field incident class, not an edge case. Traced by M11.
- **R7 — Deterministic hot path, bounded resources.** Publish/take verbs are wait-free (latest-only) or lock-free (queue), allocation-free, and bounded in worst-case execution time; benchmarks gate on tail percentiles, not means. All transport memory is sized at wiring time from declared QoS (depth × payload) — nothing grows unbounded anywhere, ever. This was implicit in the LatestMailbox contract; it is the layer's first-class obligation and holds for every reach and every verb. Traced by M1-A4, M9-A3.
- **R8 — Honest time.** Stamps use `CLOCK_MONOTONIC`; staleness and deadline semantics are **guaranteed same-host only**. Cross-host age is *advisory* — surfaced as such by the API — unless the application declares a synchronized-clock domain (PTP/NTP) in its `Domain` configuration, in which case deadline semantics apply and the declaration is recorded (in the envelope's home segment and introspection) so a post-hoc analysis knows what the numbers meant. An unsynchronized fleet must never see a confidently-wrong staleness value. Traced by M6.
- **R9 — Security is a written stance, not an omission.** v1: the inter-host reach assumes a **trusted network**; no authentication or encryption in the portable surface. The threat model documenting this assumption ships with the first Zenoh-backed release, including the revisit trigger: the first deployment whose traffic crosses a network boundary the operator does not own. Zenoh's TLS/ACL remains reachable through the R3 native escape hatch in the interim.
- **R10 — Multi-language by contract.** Components outside the family are not all C++; integrating one must not require it to be. The mechanism is layered: every portable contract is specified **at the byte level, language-neutrally** — the envelope layout, the R6 schema-hash algorithm (computable without a C++ compiler, shipped with conformance test vectors), topic naming and QoS mappings per backend, the network wire format, and the introspection segment — so a foreign-language process participates through its backend's *native* bindings (iceoryx2 and Zenoh are Rust-native with C/Python/Rust bindings) plus the spec, with no xmMessaging code at all. Dedicated bindings are built over a C ABI **when a consumer demands one**, never speculatively; the C++ API remains the primary, zero-cost surface. Language priority (decided 2026-07-10): **C++ first**, then **Python → Go → Rust** as consumers appear. Traced by M12.
- **R11 — Quantitative system evaluation, built in.** The target applications are composed navigation stacks; evaluating one (end-to-end latency, per-stage decomposition, loss accounting) must require **zero custom instrumentation** — it falls out of what the transport already records. Three mechanisms make this hold: the envelope carries *information lineage* (origin stamp + hop count, not just the publish stamp), every endpoint emits the **standard metric schema** below through the xmBase telemetry API unconditionally (family pattern: API always, SDK binding is the application's choice), and messaging stamps share the xmBase clock with telemetry records — one timeline by construction, so transport hops and application spans interleave correctly in offline analysis. Traced by M13.

## One API, three reaches

The central design commitment (decided 2026-07-10, extending the planning–control coupling design of 2026-07-06): the same typed publish/subscribe and request/response surface serves three *reaches*, selected by the application at wiring time — the call sites do not change.

| Reach | Peer is a… | Engine | Data path |
|---|---|---|---|
| **In-process** | thread | built-in (no dependencies) | move/copy through a wait-free slot or queue — no serialization |
| **Inter-process** | process on the same host | iceoryx2 (default) or the built-in **POSIX shm fallback** | true zero-copy shared memory (publisher loans, subscriber reads in place) |
| **Inter-host** | process on another host | Zenoh (no fallback — a stated divergence, not a hand-rolled transport) | serialized network transport (xmBase serialization) |

Why one surface: the 2026-07-06 planning–control design showed the coupling pattern (planner produces at ~10 Hz, controller consumes latest at 100 Hz+) is identical whether the two live in one process or two. Making reach a wiring-time property means an application can start single-process (simplest to debug), split when a deployment needs it, and distribute when a host boundary appears — without touching component code or the loop.

The in-process reach is not a degenerate case; it is the reference semantics. Every QoS contract below is defined by its in-process behavior first, and each backend must reproduce that contract (the scenario suite enforces this — see M6 in [scenarios.md](scenarios.md)).

Reach transparency is scoped by R3: the portable surface covers the contracts named in this document, and *only* those. Backend capabilities beyond them are reached through the native escape hatch, not absorbed into the API — the wrapper stays thin instead of growing toward the union of its engines. Where a backend cannot natively honor a contract, the **per-reach support matrix** records the divergence: tested, documented, and queryable at wiring time — never emulated into existence and never discovered in the field.

## The QoS vocabulary

Stated explicitly, per ADR 0006 — these are the only knobs, and every one has a defined meaning in all three reaches:

- **History**: `latest-only` (depth-1 slot, new value overwrites unread old — *the LatestMailbox contract*) or `queue<N>` (bounded FIFO, overflow policy applies).
- **Reliability**: `best-effort` (overflow drops, **counted**, never silent) or `reliable` (overflow back-pressures the publisher via an explicit return status).
- **Deadline**: the consumer-side staleness bound. The subscriber can always ask "how old is what I'm holding" and "has the deadline passed"; deadline misses are observable events, not silent staleness. Stamps are `CLOCK_MONOTONIC`; deadline semantics are guaranteed same-host, and cross-host age is advisory unless a synchronized-clock domain is declared (R8).
- **Loan**: zero-copy publication for trivially-copyable payloads (`Loan()` → construct in place → `Publish()`); in-process and iceoryx2 honor it natively, the network reach falls back to copy+serialize.
- **Ownership**: `exclusive` (default) or `shared`. Exclusive: a second `Advertise` on the topic is **refused** with an explicit status — an accidentally duplicated node cannot silently fight the real one. Shared (a deliberate declaration, e.g. planner + recovery planner): permitted, and latest-only resolves to last-writer-wins by publish stamp, documented and deterministic.

### LatestMailbox — the depth-1 contract

`history = latest-only` is the robotics workhorse (setpoints, poses, plans: only the newest matters) and gets a named contract:

1. `Publish` **never blocks and never fails for capacity reasons** — the slot is overwritten.
2. A reader takes the **newest** value or nothing; it never sees a torn or intermediate value.
3. Overwritten-unread values are **counted** (a drop metric), because "controller consistently too slow to see plans" is a diagnosis, not noise.
4. Every value is **stamped** (publish time + telemetry context), so the reader can enforce its deadline locally.

In-process this is a wait-free single-slot exchange (the original LatestMailbox). In iceoryx2 it maps to a subscriber buffer of depth 1 with overwrite; in Zenoh to keeping only the newest sample per key. The name survives the reach.

## Composition-scale contracts (decided 2026-07-10)

Rules that only show up when a whole stack is wired, stated explicitly because a navigation pipeline hits every one of them:

- **Wiring is order-independent.** `Subscribe` before the publisher exists is normal, not an error; endpoints match whenever both sides exist, whatever the process start order. Cold-start ordering (M14) is a first-class contract, distinct from crash-rejoin (M4).
- **Readiness is queryable, bounded, and application-driven.** Endpoints expose `MatchedCount()`; `Domain::WaitUntilMatched(endpoints, deadline)` is the one bounded-wait barrier verb, enough for any launcher to sequence a stack before releasing motion. There is no lifecycle framework and no match-event callback stream — that would reintroduce the hidden-execution question R3 settled.
- **Domains are isolated by key.** Every `Domain` factory takes an isolation key (default: derived from user + a configured name); two stacks on one host — dev machine, CI, twin simulations — share nothing: not topics, not shm segments, not introspection. Cross-domain visibility is only ever explicit.
- **No ordering exists across topics.** Values on one topic arrive in publish order (per publisher); *nothing* relates arrivals on different topics. A consumer gathering pose + map + goal must correlate by stamp or lineage, never by arrival interleaving.
- **One clock.** Messaging stamps and telemetry records both derive from the xmBase monotonic clock — transport hops and application spans interleave on one timeline by construction, not by calibration (R11).

## The message vocabulary — who owns which types

- **Payload types belong to the component that owns the domain.** The planning–control vocabulary (trajectory head, setpoints, mode commands) lives in xmNavigation's types tier; device-side types live in xmDriver. xmMessaging does not define robot semantics.
- **xmMessaging owns the transport vocabulary**: topic naming, QoS terms, the envelope, endpoint handles, and the wire mappings (IDL where a backend needs them). The `ros2_idl` remnants and any future wire-schema homes land here.
- Payload requirements by reach: in-process accepts any movable C++ type; zero-copy requires trivially copyable + fixed size; the network reach requires an xmBase serialization binding. The type system should make each reach's requirement a compile-time fact, not a runtime surprise.
- **Type identity is enforced, not assumed (R6)**: every endpoint carries a compile-time-derived schema hash of its payload layout; matching verifies it, a mismatch is an explicit refusal visible in introspection. Rebuild skew between processes must be impossible to hit silently.

## The envelope contract

Every message carries a fixed-size header: the xmBase telemetry context bytes (`Inject`/`Extract` — designed to ride "in any envelope"), the publish timestamp, and — decided 2026-07-10 for the pipeline case — the **information lineage**: an *origin stamp* and a *hop count*. Cross-process traces — "the planning stall and the motor fault on one timeline" — are a property of the transport, not per-application discipline. Publishing under an active trace propagates it; the subscriber side adopts the extracted context with one call.

Lineage answers navigation's real staleness question: not "how old is this plan" but "how old is the *sensor data* underneath it." A first-hop `Publish` sets origin = publish stamp, hops = 0. A component that consumes a value and publishes a derivative uses `PublishDerived(loan, upstream_sample)` — origin is preserved from the oldest consumed input, hops increment. The consumer then reads `sample.origin_age()` as easily as `sample.age()`, and a controller can refuse to act on fresh plans built from stale state — the classic fielded-stack incident, made checkable at the call site. End-to-end sensor→actuator latency becomes computable from records alone (R11). The full per-hop decomposition is *not* carried in the envelope (that would break the fixed-size contract); it is reconstructed offline from the M7 trace links.

## Back-pressure is explicit (the Aeron lesson)

`Publish` on a `reliable` endpoint returns a status — delivered, would-block, loan-exhausted — never a silent drop, and `best-effort` drops are always counted. ADR 0006 open question 6 is hereby answered **yes**: the explicit-status surface is universal. The scenario suite (M3) is the enforcement.

## Self-instrumentation and introspection (R5)

The layer is observable like everything else in the family, through the **standard metric schema** (R11): every endpoint emits a fixed instrument set via the xmBase telemetry API unconditionally, with zero application code — so any composed system produces the same dashboard shape and cross-system comparisons stay apples-to-apples.

Per publisher (`messaging.pub.<topic>.`): `publish_count`, `refused_count` (reliable would-block), `bytes`. Per subscriber (`messaging.sub.<topic>.`): `take_count`, `drop_count` (best-effort overflow), `overwrite_count` (latest-only unread), `take_age_us` histogram, `deadline_miss_count`, `queue_depth`. Per hop where both ends share a clock: `hop_latency_us` histogram. Per domain: endpoint and match counts. Instrument names, units, and labels (topic, endpoint id, pid, reach) are part of the R10 wire-contract spec — the schema is a published surface, not an implementation detail.

Beyond the in-process telemetry, the transport publishes its health counters through a shared-memory introspection segment that **any** process can read without the application's cooperation (the Aeron lesson ADR 0006 chose to steal; eCAL's monitor tooling is the usability bar). A CLI tool ships with the library: list live topics and endpoints, show per-topic rate / queue depth / drops / last-publish age, and follow a topic's health live — the first thing a user reaches for when "the controller isn't getting plans" happens on a robot.

The tool is deliberately **live-state only**: history, timelines, and post-mortem analysis belong to the telemetry plane and its offline converters into existing viewers. This boundary keeps the CLI from growing into a bespoke monitoring GUI.

## Performance benchmarks (R4)

`bench/` is a first-class tier next to `test/`: per-verb micro-benchmarks (publish, loan, take — the R3 overhead budget vs the raw backend) and per-reach message-path benchmarks (latency p50/p99/p99.9/max, throughput, jitter across payload sizes), plus the R4 robot-typical profile (tens of topics at mixed 10 Hz–1 kHz rates). Every report embeds its hardware context — CPU model, governor, kernel and RT patch, isolation, load state — so numbers are comparable or visibly not. One command runs the suite and emits a machine-readable report; CI publishes it per release and gates on regression against pinned reference numbers. Users evaluate the module on their own hardware with the same command (the ros2_tracing overhead-evaluation pattern, applied transport-wide).

## Request/response

In scope for v1 (decided 2026-07-10): typed `Client<Req, Rsp>` / `Server<Req, Rsp>` with a mandatory deadline on every call — a robot cannot wait forever on a query. In-process it is a direct handoff; iceoryx2 provides request/response natively (≥ 0.6); Zenoh provides queryables. Absent-server and deadline-expiry are explicit statuses, mirroring the pub/sub back-pressure surface. Fire-and-forget stays pub/sub; request/response is for queries and commands that need an answer (parameter reads, mode switches).

## Backend seam

The ADR 0004 pattern: a thin portable API in this library's headers; engines behind CMake options (`XMMESSAGING_WITH_ICEORYX2`, `XMMESSAGING_WITH_ZENOH`, `XMMESSAGING_WITH_POSIX_SHM`), one option per backend. The external-dependency backends default off; the POSIX shm fallback defaults **on**, because the default-off rule exists to prevent dependency creep and this backend has no dependencies to creep. The in-process reach is always present and dependency-free, so linking xmMessaging never forces a transport dependency on an application that composes in one process. DDS remains addable behind the seam if an integration contractually requires it. ROS 2 is a bridge at an application's boundary, never a backend — for the reason recorded immediately below.

### Why ROS 2 interoperability is not a reason to add a DDS backend (decided 2026-08-02)

The one-liner above was tested against a concrete proposal — add a CycloneDDS backend so that family components interoperate with the ROS 2 ecosystem (nav2, rviz) — and **confirmed, unchanged**. It is recorded here with its reasoning because the proposal is a natural one and will recur.

**Being read by a ROS 2 node requires four things to line up, not one.** The topic identifier in that RMW's convention (`rt/<name>` for the DDS RMWs; a `<domain>/<topic>/<type>/<type_hash>` key expression for `rmw_zenoh`); the type name in `pkg::msg::dds_::Type_` form; the payload being exactly a CDR-encoded ROS 2 message; and ROS 2's own type identity (RIHS01 since Iron). Our 64-byte envelope ([wire-contract §2](wire-contract.md), payload at `0x40`) breaks the third. **The other three have nothing to do with the envelope**, and two of them collide with contracts of this library: topic naming versus the domain isolation key, and R6's schema hash versus RIHS01. Solving the envelope takes you from four blockers to three.

> **Correction (2026-08-02, same day).** An earlier version of this paragraph claimed the envelope obstacle is *transport-independent* — "no choice of engine removes it… the same argument applies verbatim to a future Zenoh backend." **That was wrong, and it was asserted without checking.** Zenoh carries per-sample **attachments**, a general out-of-band metadata channel that DDS does not offer, so on a Zenoh transport the envelope could plausibly ride *beside* a pure ROS 2 payload rather than inside the type — leaving the payload bytes exactly what a ROS 2 node expects. That makes the envelope constraint **transport-dependent**, and it is the one blocker of the four that a transport can dissolve. *Confirmed 2026-08-02 by inspecting an installed `librmw_zenoh_cpp.so` (Jazzy): it uses `z_sample_attachment` / `z_query_attachment` and carries `sequence_number`, `source_timestamp` and `source_gid` there. So the side channel is real and **rmw_zenoh already occupies it** — which means an envelope would have to fit alongside their fields in their format, not simply be added. Still unverified: whether that attachment is an extensible map or a fixed struct, and the key-expression format. Both need a live check before anything is built on this.* The decision below survives the correction, but for the remaining reasons, not for the retracted one.

**Measured, 2026-08-02** (ROS 2 Jazzy, CycloneDDS 0.10.5, x86_64), because the paragraph that stood here made a structural claim that turned out to be false. A publisher built on raw CycloneDDS with a type generated by our own `idlc` was put against real ROS 2 subscribers:

| Probe | Result |
|---|---|
| `geometry_msgs::msg::dds_::Twist_` on `rt/cmd_vel`, no envelope → `ros2 topic echo`, `rmw_cyclonedds` | **works** — discovered as `geometry_msgs/msg/Twist`, values exact. Publisher shown as node `_CREATED_BY_BARE_DDS_APP_` |
| ROS 2's own type identity (RIHS01) absent | **warning, not a barrier** — `Topic type hash: INVALID`, *"Failed to parse type hash … from USER_DATA '(null)'"*, and delivery proceeds |
| Same type **plus a trailing 64-byte field**, extensibility `final` | **works** — ROS 2 deserializes its known fields correctly and ignores the extra bytes |
| Same, extensibility `appendable` | **fails** — *"offering incompatible QoS. No messages will be received"*. Loud on the ROS 2 side, silent on ours |
| Any of the above → `rmw_fastrtps` | **not established** — the topic is discovered but no data arrives, *including in the no-envelope control*, so these runs say nothing about trailing fields. Unresolved, and it matters: Fast DDS is ROS 2's default RMW |

**Retracted:** the previous claim that *"there is nowhere to put an envelope, on any transport, because the schema is not ours to extend"*. That was reasoning presented as fact, and the measurement refutes it — a standard ROS 2 message **can** carry a trailing envelope today and still be read correctly.

**The decision stands, on risk rather than impossibility.** It works *because nothing validates the structure*: matching is by type **name**, ROS 2 itself reports that it could not verify the type hash, and CDR deserialization is positional so trailing bytes are simply never reached. Three consequences follow. A ROS 2 release that enforces RIHS01 would break every such topic, silently for us. The `final`/`appendable` result shows how narrow the window is — one extensibility setting away from a total no-match. And most plainly: a type announcing itself as `geometry_msgs::msg::dds_::Twist_` while having a different structure is **a misrepresentation on the wire**, fine in a lab and a liability in a fleet, where the next tool to come along may be one that does check. Bridging keeps the standard types honest and confines the loss to a boundary we own.

**And a backend that removed the envelope would not be a reach.** It would have to drop the schema hash (R6), lineage (R11) and same-host deadline semantics (R8) to be third-party-readable — that is, drop the contracts the portable surface exists to provide, while keeping the API that promises them. R3's divergence-over-emulation permits a backend to *declare* gaps in the support matrix; it does not license one that hollows out the contracts. A component that speaks someone else's wire exactly is a **protocol adapter**, and its correctness comes from fidelity to a foreign specification rather than from our contracts. Those are different artifacts and they belong in different places.

*The distinction that keeps this compatible with the per-topic profile below:* what is refused is a **backend** — a transport choice that silently changes what every topic on it guarantees. A profile is **opt-in per topic**, declared at wiring time, with the losses visible at the call site of the application that asked for them. Same losses, opposite epistemics: one is discovered, the other is chosen.

**Worked evidence from inside the family.** The Unitree Go2 low-level interface *is* ROS 2-convention DDS — `rt/` topic mangling, `unitree_go::msg::dds_::LowCmd_` type naming, both `rmw_cyclonedds` conventions. `xmAppLeggedController` implements it directly ([ADR-0007](https://github.com/rxdu/xmAppLeggedController/blob/main/docs/adr/0007-backends-as-protocol-adapters.md) O8/A3): generated types, exact topic names, pinned QoS, **no envelope and no schema hash**. So the shape of ROS 2-convention interop is already known concretely, and it is the shape of an application-side adapter — not of a reach.

**What a bridge is, so the word carries weight.** A component at an application boundary with an xmMessaging endpoint on one side and native ROS 2 endpoints on the other, plus an **explicit per-topic mapping** between a payload type and a ROS 2 message type. The mapping cannot be generic: the type systems do not correspond, and inventing a correspondence silently is how unit and frame errors ship. Crossing it costs lineage, trace context and schema-hash identity — the bridge is lossy **by construction**, and naming it a bridge is what makes that loss declared per topic instead of discovered in the field.

**The open direction this leaves: a per-topic ROS 2 profile.** The correction above matters because it changes what is reachable for *our own* message types, as distinct from the ecosystem's. A topic could be **declared ROS 2-compatible at wiring time**, which would bind it to: a payload type laid out as a ROS 2 message, that RMW's naming convention, ROS 2's type identity, and the envelope carried however the transport allows — an attachment on Zenoh (lineage and trace **survive**), nothing on DDS (lineage and trace **lost**). That difference is precisely what R3's per-reach support matrix exists to express, so the mechanism is already in the design; nothing here requires emulation or a hollowed-out contract. It would make interoperability a declared property of a topic rather than an application-boundary afterthought, which is the right ambition.

It is **not** being built yet, for three reasons worth writing down. It buys nothing for standard messages, so a bridge is still required regardless. It introduces a second naming scheme and a second type-identity mechanism into the portable layer. And it means tracking someone else's evolving convention *inside* the layer — RIHS01 arrived in Iron, and `rmw_zenoh`'s key-expression format is young — which is exactly the churn a bridge quarantines in one rewritable component. **Revisit trigger:** a built bridge whose topics turn out to be mostly ROS 2-shaped anyway. At that point the profile is a generalization backed by evidence rather than a guess, and the family's own validate-then-lift doctrine applies unchanged.

**What this does not settle.** Whether a DDS engine should serve the *inter-host reach* on its own merits — a C dependency with no Rust toolchain, present on any machine with ROS 2 installed — remains open, but it is a question about engine selection, not about ROS 2. Note that answering it means reopening ADR 0006, which evaluated seven candidates specifically to avoid owning reliable networked QoS; a second inter-host engine must clear that ADR's bar rather than arrive as an addition to this seam. **Revisit trigger:** a deployment that needs the inter-host reach where Zenoh is unavailable or unsuitable — not a request for ROS 2 compatibility, which the bridge answers.

*Prospective consumer, noted for planning:* `xmAppLeggedController`'s gateway process (its ADR-0007 D13 isolates a public protocol endpoint from the 500 Hz control loop) is a candidate first external user of the **inter-process** reach, and is where such a ROS 2 bridge would live.

### The POSIX shm fallback backend (decided 2026-07-10)

When iceoryx2 is unavailable — unsupported target, no Rust toolchain, minimal images, pre-1.0 pin trouble — the inter-process reach must not disappear. The fallback is a built-in backend using only kernel-native primitives: named POSIX shared-memory mappings (`shm_open`) with futex wakeups. (`memfd_create` was considered at P1b and rejected: a memfd is anonymous, reachable only by fd inheritance or SCM_RIGHTS passing — which presumes a common ancestor or a broker, violating both the order-independent-wiring contract and daemonlessness; the named object is the rendezvous. Decision recorded in wire-contract §6.4 and `detail/shm_segment.hpp`.) Three facts make this affordable where it is usually reckless:

1. The portable contracts are few and small — this backend implements *our* five-knob vocabulary, not a middleware. The LatestMailbox maps to a **writer-progress-only seqlock**: readers take no locks (retry on torn reads), so a dying reader holds nothing and a dying writer leaves a skippable sequence — the M4 crash story without robust-lock recovery on the data path. Futexes are used for optional wakeups only.
2. The in-process reach already implements the same slot/ring algorithms. They are written **once**, parameterized over placement (heap vs shared mapping) and waiter (condvar vs futex) — the fallback is not a second implementation to drift.
3. The R5 introspection segment is shm machinery regardless; transport and introspection share the substrate. Lifecycle is daemonless by construction: the kernel reclaims mappings when the last mapper exits.

It ships **honestly partial** via the R3 support matrix: latest-only and best-effort queues first; `reliable` queues and request/response arrive later or remain declared divergences (`Supports()` says no; applications decide). It also repositions iceoryx2 as the performance-optimized choice rather than a single point of failure for the reach — and both backends are verified by the same M6 assertion code.

There is deliberately **no inter-host fallback**: reliable network messaging with QoS is the thing ADR 0006 evaluated seven candidates to avoid owning. Where Zenoh is unavailable, the inter-host reach is unsupported (a stated divergence) or the application bridges at its boundary.

### Why the in-process reach survives the fallback

A dependency-free shm backend raises the fair question of whether thread↔thread still needs its own reach. It does, for four reasons: (1) **type richness** — in-process accepts any movable C++ type (vectors, smart pointers, non-POD state); shm confines payloads to standard-layout, explicitly-padded, fixed-size types, a real tax on single-process applications, which are the common starting shape; (2) **it is the reference semantics** — the contracts are defined by in-process behavior and P0b proves them under ThreadSanitizer, which sees process-local memory but not cross-process shm; (3) **zero OS footprint** — no `/dev/shm` objects, no naming, no cleanup, which keeps component tests trivial in any CI sandbox; (4) **cost** — with the placement/waiter parameterization above, keeping it is nearly free: same algorithms, heap placement, richer type bound.

## Multi-language interoperability (R10)

The decisive structural fact: both engines are Rust-core projects with first-class bindings well beyond C++ (iceoryx2: Rust/C/C++, Python maturing; Zenoh: Rust/C/C++/Python and more). A non-C++ component can therefore already *speak the transport* natively — what it cannot do without our help is speak the **conventions**. So the portable value of xmMessaging is split explicitly:

- **The wire contracts** (language-neutral, versioned, the actual interop surface): envelope byte layout, schema-hash algorithm with conformance vectors, topic naming and per-backend QoS mappings, the network serialization format, and the introspection segment layout. A Python or Rust process implements these from the spec in a few dozen lines against its native backend binding.
- **The C++ library** (this repo): the typed, zero-cost realization of those contracts for the family's own components.

Constraints this feeds back into the design, all pre-API-freeze:

- The R6 schema hash is defined over the **wire layout** (field names, offsets, sizes, byte order), not over C++ type-system artifacts — otherwise no other language could ever compute it.
- Payloads on cross-language topics must be standard-layout with **explicit padding** (implicit padding bytes have unspecified content and would poison both the hash and zero-copy reads from another language); the type system should be able to assert this at wiring time.
- The envelope and introspection layouts are fixed-endian, fixed-offset structures — documented bytes, not C++ structs that happen to be shared.

Bindings policy: on demand, not speculative (the R3/no-speculation discipline), built over a small C ABI on the portable core, with the wire-contract spec keeping each one honest. Priority order **C++ → Python → Go → Rust**, with per-language realities stated up front:

- **Python** — tooling, tuners, ML-side glue; both backends have Python bindings to lean on, so spec-only participation (M12) likely precedes any packaged binding.
- **Go** — reaches the C ABI via cgo (neither backend has native Go bindings); a Go participant inherits the transport's determinism but **not** the call-site guarantees — R7's wait-free/allocation-free promise is a C++-surface property, and a garbage-collected caller cannot hold it. Fine for supervisors, fleet agents, and dashboards; not for control loops.
- **Rust** — probably needs no binding at all: the engines are Rust-native, so a Rust component uses iceoryx2/Zenoh crates directly plus the wire-contract spec — the purest validation of the spec-first strategy.

## Non-goals

- **No runtime/executor tier**: no node graph, no lifecycle management, no YAML wiring. Applications own construction, threads, and shutdown order (ADR 0005; dora-rs evaluation).
- **No discovery framework at v1**: wiring is explicit in application code or its config. Runtime discovery is ADR 0006 open question 4, deliberately deferred until a scenario demands it.
- **No robot semantics**: payload meaning belongs to components.
- **No security surface at v1** — by written stance, not omission (R9): trusted-network assumption, threat model shipped with the first Zenoh-backed release, revisit trigger defined there.

## Method and sequencing

Scenario-driven, like the telemetry stack: [scenarios.md](scenarios.md) is the executable specification. Wish-code (P0.0) defines the ideal call sites → API headers (P0a) → in-process reach + behavioral tests (P0b) → iceoryx2 backend (P1) → Zenoh backend (P2). The in-process reach ships first because it proves the API with zero dependency risk and immediately serves the planning–control coupling in single-process applications.

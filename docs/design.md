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
- **R2 — A clear, small API.** The surface stays countable: `Domain`, `Advertise`/`Subscribe`, `Publish`/`Loan`/take, `Client`/`Server`, the four QoS knobs, and the status enums — nothing else. Testable form: every public verb appears in at least one scenario, the cookbook covers every knob, and **every failure mode a user can hit has a distinct, documented status** — APIs are confusing at the error surface, not the happy path. Wish-code *is* the usability test: if it reads awkwardly, the API is wrong; deltas get recorded, not tolerated.
- **R3 — A thin wrapper.** Conventions and type safety over the engines, not a new middleware: no hidden threads, no hidden allocation, no policy magic the user didn't select. Three concrete commitments: (a) a **native escape hatch** — `Native()` on endpoints exposes the underlying backend handle, explicitly non-portable by declaration; (b) a **measured overhead budget** — wrapper cost over the raw backend is benchmarked per verb and regression-gated, not asserted; (c) **divergence over emulation** — when a backend cannot natively honor a portable contract, the library documents and tests the divergence in a per-reach support matrix (queryable at wiring time) rather than building emulation layers. Traced by M9, M6.
- **R4 — First-class performance benchmarks.** One command, on the user's own hardware, yields latency (**p50/p99/p99.9/max** — tails, not means), throughput, and jitter per reach, comparable against published reference numbers and against the raw backend. Every report captures its **hardware context** (CPU model, governor, kernel + RT patch, isolation, load state) — numbers without context are noise. The suite includes a **robot-typical profile** (tens of topics, mixed 10 Hz–1 kHz rates, small payloads) alongside single-topic sweeps, because single-topic p50 is where middleware benchmarks traditionally lie. In-tree, CI-run, results are release artifacts. Traced by M9.
- **R5 — Runtime introspection.** Diagnostics must not require rebuilding or cooperating application code: transport health (topics, endpoints, rates, queue depths, drops, staleness) is observable from *outside* the process at runtime — shared-memory counters (the Aeron idea adopted in ADR 0006) plus a CLI tool. Scope boundary: the CLI shows **live state only**; anything historical is the telemetry plane's job, offline, through existing viewers. Traced by M10.
- **R6 — Type identity across the wire.** Endpoint matching verifies payload type identity with a compile-time-derived **schema hash** (fields, offsets, sizes) carried by every endpoint; a mismatch refuses the match with an explicit status and is visible in introspection. Two processes built from different commits must find it impossible to exchange silently-reinterpreted bytes — rebuild skew is a field incident class, not an edge case. Traced by M11.
- **R7 — Deterministic hot path, bounded resources.** Publish/take verbs are wait-free (latest-only) or lock-free (queue), allocation-free, and bounded in worst-case execution time; benchmarks gate on tail percentiles, not means. All transport memory is sized at wiring time from declared QoS (depth × payload) — nothing grows unbounded anywhere, ever. This was implicit in the LatestMailbox contract; it is the layer's first-class obligation and holds for every reach and every verb. Traced by M1-A4, M9-A3.
- **R8 — Honest time.** Stamps use `CLOCK_MONOTONIC`; staleness and deadline semantics are **guaranteed same-host only**. Cross-host age is *advisory* — surfaced as such by the API — unless the application declares a synchronized-clock domain (PTP/NTP) in its `Domain` configuration, in which case deadline semantics apply and the declaration is recorded (in the envelope's home segment and introspection) so a post-hoc analysis knows what the numbers meant. An unsynchronized fleet must never see a confidently-wrong staleness value. Traced by M6.
- **R10 — Multi-language by contract.** Components outside the family are not all C++; integrating one must not require it to be. The mechanism is layered: every portable contract is specified **at the byte level, language-neutrally** — the envelope layout, the R6 schema-hash algorithm (computable without a C++ compiler, shipped with conformance test vectors), topic naming and QoS mappings per backend, the network wire format, and the introspection segment — so a foreign-language process participates through its backend's *native* bindings (iceoryx2 and Zenoh are Rust-native with C/Python/Rust bindings) plus the spec, with no xmMessaging code at all. Dedicated bindings are built over a C ABI **when a consumer demands one**, never speculatively; the C++ API remains the primary, zero-cost surface. Language priority (decided 2026-07-10): **C++ first**, then **Python → Go → Rust** as consumers appear. Traced by M12.
- **R9 — Security is a written stance, not an omission.** v1: the inter-host reach assumes a **trusted network**; no authentication or encryption in the portable surface. The threat model documenting this assumption ships with the first Zenoh-backed release, including the revisit trigger: the first deployment whose traffic crosses a network boundary the operator does not own. Zenoh's TLS/ACL remains reachable through the R3 native escape hatch in the interim.

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

### LatestMailbox — the depth-1 contract

`history = latest-only` is the robotics workhorse (setpoints, poses, plans: only the newest matters) and gets a named contract:

1. `Publish` **never blocks and never fails for capacity reasons** — the slot is overwritten.
2. A reader takes the **newest** value or nothing; it never sees a torn or intermediate value.
3. Overwritten-unread values are **counted** (a drop metric), because "controller consistently too slow to see plans" is a diagnosis, not noise.
4. Every value is **stamped** (publish time + telemetry context), so the reader can enforce its deadline locally.

In-process this is a wait-free single-slot exchange (the original LatestMailbox). In iceoryx2 it maps to a subscriber buffer of depth 1 with overwrite; in Zenoh to keeping only the newest sample per key. The name survives the reach.

## The message vocabulary — who owns which types

- **Payload types belong to the component that owns the domain.** The planning–control vocabulary (trajectory head, setpoints, mode commands) lives in xmNavigation's types tier; device-side types live in xmDriver. xmMessaging does not define robot semantics.
- **xmMessaging owns the transport vocabulary**: topic naming, QoS terms, the envelope, endpoint handles, and the wire mappings (IDL where a backend needs them). The `ros2_idl` remnants and any future wire-schema homes land here.
- Payload requirements by reach: in-process accepts any movable C++ type; zero-copy requires trivially copyable + fixed size; the network reach requires an xmBase serialization binding. The type system should make each reach's requirement a compile-time fact, not a runtime surprise.
- **Type identity is enforced, not assumed (R6)**: every endpoint carries a compile-time-derived schema hash of its payload layout; matching verifies it, a mismatch is an explicit refusal visible in introspection. Rebuild skew between processes must be impossible to hit silently.

## The envelope contract

Every message carries the xmBase telemetry context bytes (`Inject`/`Extract` — the fixed-size envelope designed to ride "in any envelope") as a standard header field, alongside the publish timestamp. Cross-process traces — "the planning stall and the motor fault on one timeline" — are a property of the transport, not per-application discipline. Publishing under an active trace propagates it; the subscriber side can adopt the extracted context with one call.

## Back-pressure is explicit (the Aeron lesson)

`Publish` on a `reliable` endpoint returns a status — delivered, would-block, loan-exhausted — never a silent drop, and `best-effort` drops are always counted. ADR 0006 open question 6 is hereby answered **yes**: the explicit-status surface is universal. The scenario suite (M3) is the enforcement.

## Self-instrumentation and introspection (R5)

The layer is observable like everything else in the family: queue depths, drop counters, hop latency histograms, and deadline-miss events are ordinary xmBase telemetry instruments (`messaging.*` namespace), emitted by the library itself with zero application code.

Beyond the in-process telemetry, the transport publishes its health counters through a shared-memory introspection segment that **any** process can read without the application's cooperation (the Aeron lesson ADR 0006 chose to steal; eCAL's monitor tooling is the usability bar). A CLI tool ships with the library: list live topics and endpoints, show per-topic rate / queue depth / drops / last-publish age, and follow a topic's health live — the first thing a user reaches for when "the controller isn't getting plans" happens on a robot.

The tool is deliberately **live-state only**: history, timelines, and post-mortem analysis belong to the telemetry plane and its offline converters into existing viewers. This boundary keeps the CLI from growing into a bespoke monitoring GUI.

## Performance benchmarks (R4)

`bench/` is a first-class tier next to `test/`: per-verb micro-benchmarks (publish, loan, take — the R3 overhead budget vs the raw backend) and per-reach message-path benchmarks (latency p50/p99/p99.9/max, throughput, jitter across payload sizes), plus the R4 robot-typical profile (tens of topics at mixed 10 Hz–1 kHz rates). Every report embeds its hardware context — CPU model, governor, kernel and RT patch, isolation, load state — so numbers are comparable or visibly not. One command runs the suite and emits a machine-readable report; CI publishes it per release and gates on regression against pinned reference numbers. Users evaluate the module on their own hardware with the same command (the ros2_tracing overhead-evaluation pattern, applied transport-wide).

## Request/response

In scope for v1 (decided 2026-07-10): typed `Client<Req, Rsp>` / `Server<Req, Rsp>` with a mandatory deadline on every call — a robot cannot wait forever on a query. In-process it is a direct handoff; iceoryx2 provides request/response natively (≥ 0.6); Zenoh provides queryables. Absent-server and deadline-expiry are explicit statuses, mirroring the pub/sub back-pressure surface. Fire-and-forget stays pub/sub; request/response is for queries and commands that need an answer (parameter reads, mode switches).

## Backend seam

The ADR 0004 pattern: a thin portable API in this library's headers; engines behind CMake options (`XMMESSAGING_WITH_ICEORYX2`, `XMMESSAGING_WITH_ZENOH`, `XMMESSAGING_WITH_POSIX_SHM`), one option per backend. The external-dependency backends default off; the POSIX shm fallback defaults **on**, because the default-off rule exists to prevent dependency creep and this backend has no dependencies to creep. The in-process reach is always present and dependency-free, so linking xmMessaging never forces a transport dependency on an application that composes in one process. DDS remains addable behind the seam if an integration contractually requires it. ROS 2 is a bridge at an application's boundary, never a backend.

### The POSIX shm fallback backend (decided 2026-07-10)

When iceoryx2 is unavailable — unsupported target, no Rust toolchain, minimal images, pre-1.0 pin trouble — the inter-process reach must not disappear. The fallback is a built-in backend using only kernel-native primitives: `memfd`-backed shared mappings with futex wakeups. Three facts make this affordable where it is usually reckless:

1. The portable contracts are few and small — this backend implements *our* four-knob vocabulary, not a middleware. The LatestMailbox maps to a **writer-progress-only seqlock**: readers take no locks (retry on torn reads), so a dying reader holds nothing and a dying writer leaves a skippable sequence — the M4 crash story without robust-lock recovery on the data path. Futexes are used for optional wakeups only.
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

# xmMessaging Library Design

- Status: **Draft** (P0.0 — scenario suite is being written; this document records the contracts the scenarios must prove)
- Date: 2026-07-10
- Governing decision: [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md) (xmMessaging: the application-level communication layer)
- Related: [ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md) (backend-seam pattern), [ADR 0005](https://github.com/rxdu/xmotion/blob/main/docs/adr/0005-application-level-composition.md) (applications own composition)

## What this library is

xmMessaging (`xmotion::messaging`) moves typed data between the components an application composes. It is the glue tier of ADR 0005 made concrete: algorithm components (xmNavigation) and hardware components (xmDriver) keep producing and consuming plain xmBase types and **never link this library**; applications link it to wire those components into a running system.

## One API, three reaches

The central design commitment (decided 2026-07-10, extending the planning–control coupling design of 2026-07-06): the same typed publish/subscribe and request/response surface serves three *reaches*, selected by the application at wiring time — the call sites do not change.

| Reach | Peer is a… | Engine | Data path |
|---|---|---|---|
| **In-process** | thread | built-in (no dependencies) | move/copy through a wait-free slot or queue — no serialization |
| **Inter-process** | process on the same host | iceoryx2 | true zero-copy shared memory (publisher loans, subscriber reads in place) |
| **Inter-host** | process on another host | Zenoh | serialized network transport (xmBase serialization) |

Why one surface: the 2026-07-06 planning–control design showed the coupling pattern (planner produces at ~10 Hz, controller consumes latest at 100 Hz+) is identical whether the two live in one process or two. Making reach a wiring-time property means an application can start single-process (simplest to debug), split when a deployment needs it, and distribute when a host boundary appears — without touching component code or the loop.

The in-process reach is not a degenerate case; it is the reference semantics. Every QoS contract below is defined by its in-process behavior first, and each backend must reproduce that contract (the scenario suite enforces this — see M6 in [scenarios.md](scenarios.md)).

## The QoS vocabulary

Stated explicitly, per ADR 0006 — these are the only knobs, and every one has a defined meaning in all three reaches:

- **History**: `latest-only` (depth-1 slot, new value overwrites unread old — *the LatestMailbox contract*) or `queue<N>` (bounded FIFO, overflow policy applies).
- **Reliability**: `best-effort` (overflow drops, **counted**, never silent) or `reliable` (overflow back-pressures the publisher via an explicit return status).
- **Deadline**: the consumer-side staleness bound. The subscriber can always ask "how old is what I'm holding" and "has the deadline passed"; deadline misses are observable events, not silent staleness.
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

## The envelope contract

Every message carries the xmBase telemetry context bytes (`Inject`/`Extract` — the fixed-size envelope designed to ride "in any envelope") as a standard header field, alongside the publish timestamp. Cross-process traces — "the planning stall and the motor fault on one timeline" — are a property of the transport, not per-application discipline. Publishing under an active trace propagates it; the subscriber side can adopt the extracted context with one call.

## Back-pressure is explicit (the Aeron lesson)

`Publish` on a `reliable` endpoint returns a status — delivered, would-block, loan-exhausted — never a silent drop, and `best-effort` drops are always counted. ADR 0006 open question 6 is hereby answered **yes**: the explicit-status surface is universal. The scenario suite (M3) is the enforcement.

## Self-instrumentation

The layer is observable like everything else in the family: queue depths, drop counters, hop latency histograms, and deadline-miss events are ordinary xmBase telemetry instruments (`messaging.*` namespace), emitted by the library itself with zero application code.

## Request/response

In scope for v1 (decided 2026-07-10): typed `Client<Req, Rsp>` / `Server<Req, Rsp>` with a mandatory deadline on every call — a robot cannot wait forever on a query. In-process it is a direct handoff; iceoryx2 provides request/response natively (≥ 0.6); Zenoh provides queryables. Absent-server and deadline-expiry are explicit statuses, mirroring the pub/sub back-pressure surface. Fire-and-forget stays pub/sub; request/response is for queries and commands that need an answer (parameter reads, mode switches).

## Backend seam

The ADR 0004 pattern: a thin portable API in this library's headers; engines behind CMake options (`XMMESSAGING_WITH_ICEORYX2`, `XMMESSAGING_WITH_ZENOH`), one option per backend, all default-off. The in-process reach is always present and dependency-free, so linking xmMessaging never forces a transport dependency on an application that composes in one process. DDS remains addable behind the seam if an integration contractually requires it. ROS 2 is a bridge at an application's boundary, never a backend.

## Non-goals

- **No runtime/executor tier**: no node graph, no lifecycle management, no YAML wiring. Applications own construction, threads, and shutdown order (ADR 0005; dora-rs evaluation).
- **No discovery framework at v1**: wiring is explicit in application code or its config. Runtime discovery is ADR 0006 open question 4, deliberately deferred until a scenario demands it.
- **No robot semantics**: payload meaning belongs to components.

## Method and sequencing

Scenario-driven, like the telemetry stack: [scenarios.md](scenarios.md) is the executable specification. Wish-code (P0.0) defines the ideal call sites → API headers (P0a) → in-process reach + behavioral tests (P0b) → iceoryx2 backend (P1) → Zenoh backend (P2). The in-process reach ships first because it proves the API with zero dependency risk and immediately serves the planning–control coupling in single-process applications.

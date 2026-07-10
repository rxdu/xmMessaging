# xmMessaging

Typed, low-latency communication for composing XMotion components into running systems — the application-level glue tier decided in [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md).

**Status: P0.0 — scenario suite (wish-code) in progress. There is no API yet; the scenarios are being written to define it.**

## What it will be

One typed publish/subscribe + request/response surface across three *reaches*, chosen by the application at wiring time:

| Reach | Engine | Data path |
|---|---|---|
| in-process (thread ↔ thread) | built-in, dependency-free | wait-free slot/queue, no serialization |
| inter-process (same host) | [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) | true zero-copy shared memory |
| inter-host | [Zenoh](https://zenoh.io/) | serialized network transport |

Key contracts (see [docs/design.md](docs/design.md)):

- **LatestMailbox** — the depth-1 latest-only semantics every reach must honor: non-blocking overwrite, newest-or-nothing reads, counted overwrites, stamped values with consumer-side deadlines.
- **Envelope** — every message carries the xmBase telemetry context; cross-process traces are a transport property.
- **Explicit back-pressure** — `Publish` returns a status; drops are counted, never silent.
- **Backend seam** — engines behind CMake options, all default-off; the in-process reach never costs a dependency.

Per [ADR 0005](https://github.com/rxdu/xmotion/blob/main/docs/adr/0005-application-level-composition.md), algorithm and hardware components never link this library — only applications do.

## Layout

- [docs/design.md](docs/design.md) — library design (contracts, QoS vocabulary, reach model)
- [docs/scenarios.md](docs/scenarios.md) — the scenario suite: the executable specification the API is built to satisfy
- `test/scenarios/` — wish-code (P0.0), later the behavioral acceptance tests

## Part of XMotion

xmMessaging depends on [xmBase](https://github.com/rxdu/xmBase) only and is assembled with the family in the [xmotion](https://github.com/rxdu/xmotion) superbuild as `components/messaging`.

## License

Apache-2.0 — see [LICENSE](LICENSE).

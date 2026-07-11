# Changelog

## v0.1.0 — 2026-07-11

First release: the typed messaging surface with two real reaches. Covers phases P0.0 (wish-code spec), P0a (API headers), P0b (in-process reach), P1b (POSIX-shm inter-process backend), plus the M10 introspection and M11 rebuild-skew milestones pulled forward against the shm backend.

### What is in

- **The portable API (R2)**: `Domain` with per-backend factories and isolation keys, `Advertise`/`Subscribe`, `Publish`/`Loan`/`PublishDerived`, `TakeLatest`/`TryTake`, `Client`/`Server` request/response with mandatory deadlines, `WaitUntilMatched`, `Supports()`, the five QoS knobs, and the status enums — construction never throws, every failure mode has a distinct documented status (see [docs/cookbook.md](docs/cookbook.md)).
- **In-process reach** (the reference semantics): wait-free seqlock LatestMailbox, lock-free SPSC bounded queues, RPC slot machinery — full contract set, allocation-free hot path (R7), TSan-clean.
- **POSIX-shm inter-process reach** (dependency-free, kernel primitives only): named segments + futex wakeups, daemonless, crash-safe reads, pid-liveness reclaim, never-unlink lifecycle with `xmmsg clean`.
- **Introspection (R5)**: per-topic shared-memory counters readable with zero application cooperation; the `xmmsg` CLI (`list`/`stat`/`watch`/`clean`, `--json`).
- **Type identity (R6)**: schema-hash matching at wiring time; rebuild skew between processes is refused with `kTypeMismatch`, recorded in the segment header, and rendered by `xmmsg stat`.
- **Lineage & evaluation (R11)**: envelope carries origin stamp + hop count; `origin_age()` distinguishes fresh values built from stale information; the wire-contract §7 metric schema is emitted through the xmBase telemetry API unconditionally.
- **Benchmarks (R4/M9)**: `scripts/bench.sh` — one command, machine-readable JSON report with hardware context; alloc-gated; report-only comparison until CI pins reference numbers.
- **Packaging (R1)**: install/export rules with a CMake config-file package (`find_package(xmMessaging)` → `xmotion::xmmessaging`), `libxmotion-messaging` deb via CPack (family pattern), quickstart script tested in CI.
- **Verification**: 63 behavioral tests × 3 suites (plain, TSan, ASan) on Ubuntu 22.04/24.04; aarch64 CI leg (the seqlock on a weak memory model); M8 dependency-closure gates (lib-only link, `ldd` clean, backends default-off).

### The reach matrix at v0.1.0

| Contract | in-process | POSIX shm |
|---|---|---|
| latest-only (LatestMailbox) | yes | yes |
| bounded queue (best-effort) | yes | yes (depth ≤ 16) |
| reliable queue (back-pressure) | yes | **no — declared divergence**, `Advertise` refused |
| zero-copy loan | yes (native) | **no — declared divergence**: `Loan` works, publication copies (the copy is what makes reads crash-safe) |
| late-join warm start | yes | yes |
| request/response | yes | **no — declared divergence**, wiring refused |
| deadline (measured staleness) | yes | yes (same host, one clock) |
| shared ownership | yes | **no — declared divergence**, `Advertise` refused |

iceoryx2 (P1) and Zenoh (P2) factories exist so applications can pin intent, but their endpoints carry `kUnsupportedReach` — never a silent in-process fallback.

### Known divergences and deferrals

- **Reliable queues and request/response are not on the shm reach** — `Supports()` says no and wiring refuses (divergence-over-emulation, R3); implement-or-declare-permanent is a P1 decision (wire-contract §6.3/§6.4).
- **Interim schema hash for undescribed types**: payload types without the `XMMSG_DESCRIBE` opt-in hash over `typeid` name + size + align — deterministic and skew-refusing in practice, but not computable from another language and blind to a pure field reorder (M11-A2). The canonical §4.2 wire-layout hash is available today via `XMMSG_DESCRIBE` (wire-contract §6.4 records the divergence).
- **§7 metric labels/units** are not expressible through the current xmBase metric API — instrument names carry the topic; label plumbing lands with the xmBase metric-API extension.
- `kUnsupportedReach` currently means both "backend not compiled in" and "contract unsupported on this reach" — splitting the two is a P1 API decision.
- Segment per-record bytes are not yet the §2 64-byte envelope frame (aligned before M12 targets this backend).
- M9 reference numbers are report-only until CI pins `bench/reference.json` (M9-A5).

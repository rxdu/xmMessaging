<h1 align="center">
  <img src="docs/xmmessaging.svg" width="96" alt="xmMessaging"><br>
  xmMessaging&nbsp;·&nbsp;π
</h1>

Typed, low-latency communication for composing XMotion components into running systems — the application-level glue tier decided in [ADR 0006](https://github.com/rxdu/xmotion/blob/main/docs/adr/0006-messaging-layer.md).

**Status: v0.1.0 — the in-process and POSIX-shm reaches are real.** The v0.1 gate (docs/scenarios.md) is met: 63 behavioral tests pass on three suites (plain, ThreadSanitizer, AddressSanitizer), CI covers Ubuntu 22.04/24.04 on x86_64 **and aarch64** (the seqlock meets a weak memory model there, not just x86-TSO), the M9 benchmark tier ships its machine-readable report, and the `xmmsg` introspection CLI (M10) plus rebuild-skew refusal (M11) landed against the POSIX-shm backend. See [CHANGELOG.md](CHANGELOG.md) for exactly what is in and what is deferred.

## Quickstart — first message in under 10 minutes

From a clean Ubuntu 22.04/24.04 machine (the R1 claim — CI runs [`scripts/quickstart.sh`](scripts/quickstart.sh), which is exactly these steps, from a clean checkout on every change, so this section cannot drift from what is verified):

```bash
# 1. Dependencies: compiler, CMake, Eigen (used by the bundled xmBase).
sudo apt-get install -y build-essential cmake libeigen3-dev git

# 2. Clone with submodules (xmBase rides along; an installed
#    libxmotion-base is used instead when present).
git clone --recurse-submodules https://github.com/rxdu/xmMessaging.git
cd xmMessaging

# 3. One configure + build (tests on).
cmake -S . -B build-quickstart -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-quickstart -j"$(nproc)"

# 4. First message: the M8 lib-only example — one in-process
#    publish/take round trip, linking only the library.
./build-quickstart/test/link/xmmsg_libonly_link_test

# 5. Your topic, visible from OUTSIDE the process (R5): one POSIX-shm
#    publish, then xmmsg lists it — type hash, QoS, publisher liveness,
#    last-publish age. (The isolation key is user-derived: u<uid>.<name>.)
./build-quickstart/test/behavioral/shm_test_helper publish_once quickstart demo.plan.head 42
./build-quickstart/tools/xmmsg/xmmsg list --domain "u$(id -u).quickstart"
./build-quickstart/tools/xmmsg/xmmsg clean --domain "u$(id -u).quickstart" --yes
```

Or run all of it as one command: `./scripts/quickstart.sh`. Then read the [cookbook](docs/cookbook.md) — a recipe for every knob and every status the API can hand you.

## One API, three reaches

One typed publish/subscribe + request/response surface across three *reaches*, chosen by the application at wiring time:

| Reach | Engine | Data path | v0.1.0 |
|---|---|---|---|
| in-process (thread ↔ thread) | built-in, dependency-free | wait-free slot/queue, no serialization | **shipped** — the reference semantics, full contract set |
| inter-process (same host) | built-in POSIX shm fallback | shared memory (seqlock slot / SPSC ring) | **shipped, honestly partial** — latest-only, best-effort queue, warm start, deadline; reliable queue / RPC / loan / shared ownership are declared divergences (`Supports()` says no, wiring refuses) |
| inter-process (same host) | [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) | true zero-copy shared memory | P1 — factory exists, endpoints carry `kUnsupportedReach` |
| inter-host | [Zenoh](https://zenoh.io/) | serialized network transport | P2 — factory exists, endpoints carry `kUnsupportedReach` |

Key contracts (see [docs/design.md](docs/design.md)):

- **LatestMailbox** — the depth-1 latest-only semantics every reach must honor: non-blocking overwrite, newest-or-nothing reads, counted overwrites, stamped values with consumer-side deadlines.
- **Envelope** — every message carries the xmBase telemetry context plus information lineage (origin stamp, hop count); cross-process traces are a transport property.
- **Explicit back-pressure** — `Publish` returns a status; drops are counted, never silent.
- **Backend seam** — engines behind CMake options; the external-dependency backends default off, the dependency-free POSIX shm fallback defaults on; the in-process reach never costs a dependency.
- **Thin by measurement** — wrapper overhead vs the raw engines is benchmarked (`scripts/bench.sh`, M9), with a native escape hatch for anything the portable surface doesn't cover; reference numbers ship with every release.
- **Introspectable** — transport health (topics, rates, drops, staleness) is readable from outside the process at runtime via `xmmsg`, with zero application cooperation.
- **Type identity enforced** — endpoint matching verifies a schema hash of the payload layout; rebuild skew between processes is refused with a distinct status, visible in `xmmsg stat` (R6/M11).
- **Multi-language by contract** — every portable contract is specified at the byte level ([docs/wire-contract.md](docs/wire-contract.md)), so non-C++ components join through their backend's native bindings plus the spec; dedicated bindings only when a consumer demands one (priority: C++ first, then Python, Go, Rust).

Per [ADR 0005](https://github.com/rxdu/xmotion/blob/main/docs/adr/0005-application-level-composition.md), algorithm and hardware components never link this library — only applications do.

## Layout

- [docs/design.md](docs/design.md) — library design (contracts, QoS vocabulary, reach model)
- [docs/cookbook.md](docs/cookbook.md) — practical recipes: every QoS knob, every verb, every status enum value with what-to-do (R2)
- [docs/scenarios.md](docs/scenarios.md) — the scenario suite: the executable specification the API is built to satisfy
- [docs/wire-contract.md](docs/wire-contract.md) — the language-neutral byte-level contracts (envelope, schema hash, segment layout, metric schema)
- `test/behavioral/` — the behavioral acceptance suite (the scenarios, compiled and asserted; `test/scenarios/` keeps the original wish-code as the API spec)
- `bench/` — the M9 benchmark tier; `scripts/bench.sh` is the one command
- `tools/xmmsg/` — the `xmmsg` introspection CLI (R5): `list` / `stat` / `watch` live topics — type hash, QoS, pids, counters, last-publish age — from outside the observed processes, plus `clean` for explicit segment reclaim; `--json` for machine consumption

## Install / packaging

`cmake --install` ships the static library, the public headers, the `xmmsg` CLI, and a CMake config-file package, so downstream wiring is two lines (family pattern, same as xmBase/xmTelemetry):

```cmake
find_package(xmMessaging 0.1.0 REQUIRED)
target_link_libraries(my_app PRIVATE xmotion::xmmessaging)
```

`cpack` in the build directory produces the `libxmotion-messaging` deb (depends on `libxmotion-base`, installs under `/opt/xmotion` like the rest of the family).

## Part of XMotion

xmMessaging depends on [xmBase](https://github.com/rxdu/xmBase) only and is assembled with the family in the [xmotion](https://github.com/rxdu/xmotion) superbuild as `components/messaging`.

## License

Apache-2.0 — see [LICENSE](LICENSE).

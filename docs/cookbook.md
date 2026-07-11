# xmMessaging Cookbook

- Status: current as of **v0.1.0** (in-process + POSIX-shm reaches)
- Scope: one recipe per knob and per verb, plus the complete status reference — R2's testable form: *the cookbook covers every knob, and every failure mode a user can hit has a distinct, documented status*.

Every recipe is adapted from the behavioral acceptance suite (`test/behavioral/`), which compiles and passes on every CI run — the code shapes here are the tested call sites, not invented examples. Include one header, link one target:

```cpp
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;
```

```cmake
find_package(xmMessaging 0.1.0 REQUIRED)
target_link_libraries(my_app PRIVATE xmotion::xmmessaging)
```

## 1. Wiring a Domain — reach, isolation, capability

A `Domain` is the application's messaging session: it owns the reach (chosen by which factory you call — the endpoint call sites never change) and an isolation key (D17) so two stacks on one host share nothing.

```cpp
// In-process: threads in one process. Dependency-free, the reference
// semantics — every contract is defined by this reach's behavior.
auto domain = msg::Domain::InProcess({.name = "nav_stack"});

// Inter-process, same host: the built-in POSIX-shm backend (v0.1.0) —
// dependency-free, daemonless. Same name in two processes = one domain.
auto ipc = msg::Domain::PosixShm({.name = "nav_stack"});

// Inter-process via iceoryx2 (P1) / inter-host via Zenoh (P2): the
// factories exist so wiring code can pin intent today, but in v0.1.0
// every endpoint they mint carries kUnsupportedReach — never a silent
// in-process fallback.
auto iox = msg::Domain::Iceoryx2({.name = "nav_stack", .service_name = "nav"});
auto net = msg::Domain::Zenoh({.name = "nav_stack",
                               .locator = "tcp/robot-nav:7447",
                               .clock = msg::ClockDomain::kUnsynced});
```

Isolation keys: every factory takes a `name`; an empty name derives a default from the user. Two domains with different keys never see each other's topics, segments, or introspection — dev machine, CI, and twin simulations coexist on one host (M14-A4). Two `Domain` handles with the *same* key are one domain. On the shm reach the full key is user-prefixed: `u<uid>.<name>` — that is the key `xmmsg --domain` matches.

Not every reach honors every contract (R3: divergence over emulation). Query the support matrix at wiring time instead of discovering a divergence in the field (M6-A6):

```cpp
if (!domain.Supports(msg::Contract::kReliableQueue)) {
  // The POSIX-shm reach refuses reliable queues at Advertise — decide HERE
  // whether best-effort + drop accounting is acceptable for this topic.
}
```

`Contract` values: `kLatestOnly`, `kBoundedQueue`, `kReliableQueue`, `kZeroCopyLoan`, `kLateJoinWarmStart`, `kRequestResponse`, `kDeadline`, `kSharedOwnership`.

Lifecycle: `Domain` is move-only; endpoints must not outlive the Domain that minted them; teardown of any endpoint is scope exit — RAII, no unsubscribe ceremony (D7).

## 2. Latest-only pub/sub with the freshness switch (the M1 pattern)

The robotics workhorse: a planner produces at ~10 Hz, a control loop consumes the newest plan at 1 kHz and must know how stale it is. `history = latest-only` is the LatestMailbox contract — `Publish` never blocks and never fails for capacity (the slot is overwritten, overwrites are counted), a reader takes the newest value or nothing, never a torn value.

Wiring (endpoints are acquired at wiring time and used on the hot path — never name-lookup inside a loop):

```cpp
struct TrajectoryHead {  // payload types belong to YOUR component, not here
  std::uint64_t plan_id;
  double x[8], y[8], v[8], t_offset[8];
};

auto pub = domain.Advertise<TrajectoryHead>(
    "nav.plan.head", {.history = msg::History::LatestOnly()});
auto sub = domain.Subscribe<TrajectoryHead>(
    "nav.plan.head",
    {.history = msg::History::LatestOnly(), .deadline = 250ms});
// D18: construction never throws — CHECK THE HANDLE STATUS.
assert(pub.status() == msg::AdvertiseStatus::kOk);
assert(sub.status() == msg::SubscribeStatus::kOk);
```

Publisher side — plain copy publish, or the zero-copy loan path (the Loan QoS knob is a verb, not a field):

```cpp
TrajectoryHead plan{/*...*/};
pub.Publish(plan);  // latest-only: always kOk, never blocks (M1-A1)

// Zero-copy: construct in place in transport memory, then hand it back.
// Requires a trivially copyable payload (compile-time checked at Loan()).
auto loan = pub.Loan();
if (loan.status() == msg::LoanStatus::kOk) {
  loan->plan_id = 43;  // construct directly in the slot
  pub.Publish(std::move(loan));
}  // an unpublished loan returns to the pool at scope exit (RAII)
```

Consumer side — the freshness switch. `TakeLatest()` always returns a `Sample`; emptiness and staleness are *freshness states*, not different return types, so the call site cannot skip handling them (D1/D2):

```cpp
auto plan = sub.TakeLatest();
switch (plan.freshness()) {
  case msg::Freshness::kFresh:
    // Newest value, within the declared 250 ms deadline. Act on it.
    Track(*plan);
    break;
  case msg::Freshness::kStale:
    // A value exists but exceeded the deadline — "lost the planner".
    // Still accessible (*plan is valid); its age() tells the truth.
    // Degrade: hold, slow down, or fall back — YOUR policy, made explicit.
    HoldPosition();
    break;
  case msg::Freshness::kNone:
    // Never received anything — "never had a plan", a DIFFERENT incident
    // than kStale (normal before matching at cold start, M14-A1).
    // Dereferencing a kNone sample is a contract violation (debug-assert).
    WaitForFirstPlan();
    break;
}
```

Rules that make this dependable: no deadline declared → `kStale` never applies; a late joiner's first take warm-starts from the current slot value with its ORIGINAL stamp, so `age()` never reports ~0 for old data (M2-A1, D6); `age()` is always available for custom staleness logic regardless of the declared deadline; the Fresh→Stale transition also fires the `messaging.sub.<topic>.deadline_miss_count` event — one event per transition, not per stale take.

Accounting (D9 — the same atomics feed the `messaging.*` instruments): `unique_seen + OverwriteCount(sub) == published`, exactly — "controller consistently too slow to see plans" is a diagnosis, not noise:

```cpp
std::uint64_t missed = msg::introspect::OverwriteCount(sub);
std::uint64_t taken = msg::introspect::TakeCount(sub);
```

## 3. Bounded queues and reliability — back-pressure is explicit

When every value matters (events, commands), declare `queue<N>` history. All transport memory is sized at wiring time from this declaration (R7) — nothing grows unbounded, so overflow is a real, decidable event. The Reliability knob decides *who* absorbs overflow.

Best-effort (default): `Publish` always returns `kOk` — the transport accepted it; whether each subscriber kept it is that subscriber's queue policy, counted in *its* drop counter (D8). Overflow drops the incoming value (drop-newest), so what was already accepted is never displaced:

```cpp
auto pub = domain.Advertise<Event>(
    "nav.events", {.history = msg::History::Queue(8),
                   .reliability = msg::Reliability::kBestEffort});
auto sub = domain.Subscribe<Event>("nav.events",
                                   {.history = msg::History::Queue(8)});

pub.Publish(event);  // always kOk on best-effort

// Drain: TryTake pops the oldest queued value, kNone when empty.
for (;;) {
  auto ev = sub.TryTake();
  if (ev.freshness() == msg::Freshness::kNone) break;
  Handle(*ev);
}
// The Aeron lesson, exact: delivered + DropCount(sub) == published (M3-A2).
```

Reliable: overflow back-pressures the *publisher* via an explicit status. `Publish` never blocks internally — `kWouldBlock` means the queue is full RIGHT NOW and nothing was enqueued; retry, coalesce, or shed is caller policy, stated at the call site (M3-A1):

```cpp
auto rpub = domain.Advertise<Event>(
    "nav.critical", {.history = msg::History::Queue(8),
                     .reliability = msg::Reliability::kReliable});

switch (rpub.Publish(event)) {
  case msg::PublishStatus::kOk:
    break;
  case msg::PublishStatus::kWouldBlock:
    // Full now, nothing enqueued. Refusal is a NOW fact, not a latch —
    // capacity returns as the consumer drains. Pick a policy:
    //   retry after backoff | coalesce into the next event | shed + count
    OnBackpressure(event);
    break;
}
```

Notes: `kWouldBlock` never occurs on latest-only (the slot is overwritten instead); reliable queues never drop (`DropCount` stays 0 — refusals are the publisher's `RefusedCount`); on the POSIX-shm reach reliable queues are a declared divergence — `Supports(kReliableQueue)` is false and the reliable `Advertise` is refused with `kUnsupportedReach` (recipe 1); shm best-effort queues support depth ≤ 16 — a deeper declaration is refused at wiring, never silently clamped.

## 4. Request/response with deadline statuses

For queries and commands that need an answer (parameter reads, mode switches). The deadline is mandatory — there is deliberately no deadline-less `Call` overload; an unbounded wait is unrepresentable (M5-A4). Fire-and-forget stays pub/sub.

Server — a take/reply endpoint polled from a loop *you* own; the library runs no hidden threads (R3/D10). `WaitForWorkOrShutdown` is the bounded park: neither busy-spin nor blind sleep:

```cpp
auto server = domain.Serve<GainsRequest, GainsResponse>("nav.config.get_gains");
assert(server.status() == msg::AdvertiseStatus::kOk);  // one server per topic

while (!shutdown_requested) {
  for (;;) {  // drain every pending request before parking again
    auto req = server.TakeRequest();
    if (req.freshness() == msg::Freshness::kNone) break;  // inbox empty
    msg::ReplyStatus rs = server.Reply(req, GainsFor(req->mode));
    if (rs == msg::ReplyStatus::kExpired) {
      // The caller's deadline already passed; the reply was discarded by
      // correlation. Observable, not an error — count it if you care.
    }
  }
  server.WaitForWorkOrShutdown(5ms);  // returns early when work arrives
}
```

Client — `Call` blocks at most until the deadline, and the three outcomes are distinct incidents with distinct recoveries (M5-A2/A3):

```cpp
auto client = domain.Client<GainsRequest, GainsResponse>("nav.config.get_gains");

auto result = client.Call(GainsRequest{2}, 500ms);
switch (result.status()) {
  case msg::CallStatus::kOk:
    Apply(*result);  // typed response; deref valid only on kOk
    break;
  case msg::CallStatus::kDeadlineExpired:
    // The server exists but did not answer in time — "overloaded".
    // The late reply is discarded by correlation and NEVER surfaces on a
    // later call. Retry with a longer deadline, or degrade.
    break;
  case msg::CallStatus::kNoServer:
    // Nobody serves this topic — "crashed / not started". Fails FAST
    // (does not wait out the deadline). Check the launcher, use the
    // readiness barrier (recipe 6) before releasing motion.
    break;
}
```

Trace continuity rides free (D13/M5-A1): `Call` snapshots the calling thread's active telemetry context into the request envelope; the handler reads it with `req.context()` and replies under a `telemetry::ContextGuard` of it to link the server span into the caller's trace. In-flight bound: 8 calls per topic are preallocated (v0.1.0 fixed); surplus concurrent callers park until a slot frees, bounded by their own deadlines. Request/response is in-process-only at v0.1.0 — on the shm reach `Serve`/`Client` wiring is refused (`Supports(kRequestResponse)` is false).

## 5. Lineage through a pipeline stage — PublishDerived

Navigation's real staleness question is not "how old is this plan" but "how old is the *sensor data* underneath it". The envelope carries information lineage (origin stamp + hop count); a stage that consumes a value and publishes a derivative uses `PublishDerived`, and the lineage arithmetic is the library's job — zero bookkeeping in your component (D14, M13):

```cpp
// Stage: estimator — consumes a pose sample, publishes a derived estimate.
auto pose = pose_sub.TakeLatest();
if (pose.freshness() != msg::Freshness::kNone) {
  auto loan = state_pub.Loan();
  if (loan.status() == msg::LoanStatus::kOk) {
    Fuse(*pose, &*loan);  // fill the estimate from the pose
    // Origin stamp is preserved from the OLDEST consumed input (variadic:
    // pass every upstream sample); hop count increments.
    state_pub.PublishDerived(std::move(loan), pose);
  }
}
```

First-hop rule: a plain `Publish` starts lineage free — origin stamp == publish stamp, hops == 0 (M13-A2). Downstream, the consumer reads both ages and can refuse to act on a fresh value built from stale information — the classic fielded-stack incident, made checkable at the call site (M13-A1):

```cpp
auto cmd = cmd_sub.TakeLatest();
constexpr auto kMaxInformationAge = 150ms;
if (cmd.freshness() == msg::Freshness::kFresh &&
    cmd.origin_age() > kMaxInformationAge) {
  // The value is fresh (age() is small — just published) but the SENSOR
  // DATA underneath it is old (origin_age() grew through the stall).
  // age() alone would lie here.
  Degrade();
}
```

End-to-end sensor→actuator latency is computable from the envelope stamps at the consumer alone — `cmd.origin_age()` *is* that computation (M13-A3) — because messaging stamps and telemetry records share the one xmBase monotonic clock (R11). The full per-hop decomposition is reconstructed offline from the M7 trace links, not carried in the envelope.

## 6. Readiness barrier at stack startup

Wiring is order-independent: `Subscribe` before the publisher exists is normal, not an error — freshness is simply `kNone` until matching happens (M14-A1). What a launcher needs before releasing motion is the one bounded barrier verb (D16); there is no lifecycle framework and no match-event callback stream:

```cpp
auto est_sub = domain.Subscribe<StateEstimate>("nav.estimator.state", {});
auto cmd_pub = domain.Advertise<CmdSample>("nav.controller.cmd", {});

switch (domain.WaitUntilMatched({&est_sub, &cmd_pub}, 10s)) {
  case msg::WaitStatus::kMatched:
    // Every listed endpoint has >= 1 peer. Release motion.
    break;
  case msg::WaitStatus::kDeadlineExpired:
    // At least one endpoint is unmatched — bounded, never a hang, never
    // fake success. WHO is missing is queryable per endpoint:
    if (est_sub.MatchedCount() == 0) Report("estimator did not come up");
    if (cmd_pub.MatchedCount() == 0) Report("no consumer for commands");
    Abort();
    break;
}
```

`MatchedCount()` keeps tracking joins and leaves after startup — a peer dying drops it, a restart raises it again with no re-wiring code on your side (M14-A5).

## 7. Ownership — exclusive refusal, shared declaration

Ownership defaults to exclusive: a second `Advertise` on the topic is refused with a distinct status, so an accidentally duplicated node cannot silently fight the real one (D15, M14-A3):

```cpp
auto pub = domain.Advertise<StateEstimate>("nav.estimator.state", {});
if (pub.status() == msg::AdvertiseStatus::kOwnershipRefused) {
  // Another publisher already owns this topic — the incumbent keeps
  // working, untouched. Almost always a launcher bug: the same node
  // started twice. Log which topic and exit; do NOT retry into the
  // refusal. (Ownership is released with the endpoint — a clean restart
  // re-advertises successfully, M14-A5.)
}
```

Shared ownership is a deliberate declaration made by BOTH publishers (e.g. planner + recovery planner). Mixing is refused — an exclusive incumbent refuses a shared newcomer and vice versa. On latest-only, shared resolves last-writer-wins by publish stamp: deterministic, monotone at the subscriber, never a torn interleaving of two writers:

```cpp
auto primary = domain.Advertise<Setpoint>(
    "nav.control.setpoint", {.ownership = msg::Ownership::kShared});
auto recovery = domain.Advertise<Setpoint>(
    "nav.control.setpoint", {.ownership = msg::Ownership::kShared});
// Both kOk; a subscriber's TakeLatest always resolves to the newest stamp.
```

On the POSIX-shm reach shared ownership is a declared divergence (no robust cross-process writer serialization by design): `Supports(kSharedOwnership)` is false and the shared `Advertise` is refused.

## 8. Reading your topic with xmmsg during bring-up

When "the controller isn't getting plans" happens on a robot, diagnosis must not require rebuilding, instrumenting, or even cooperating application code (R5). `xmmsg` attaches read-only to the POSIX-shm segments of *running* (or crashed) processes:

```console
$ xmmsg list --domain u1000.nav_stack
DOMAIN            TOPIC           TYPE-HASH           QOS            PAYLOAD   PUB-PID  PUB    SUBS  LAST-PUB
u1000.nav_stack   nav.plan.head   0xE1EC928B9042438B  latest-only        320     41772  live      1  102 ms
```

- `xmmsg list [--domain KEY] [--json]` — every live topic: type hash, QoS, payload size, publisher pid + liveness, subscriber count, last-publish age. The shm domain key is user-prefixed: `u<uid>.<name>`.
- `xmmsg stat <topic> [--domain KEY] [--json]` — one topic in full: identity, per-endpoint counters (publish/take/drop/overwrite/deadline-miss), accepted ordinal, last-publish age, and the type-mismatch refusal record when rebuild skew was refused (M11 — both hashes shown).
- `xmmsg watch <topic> [--interval-ms N]` — `stat` re-printed on an interval; plain stdout, pipeable.
- `xmmsg clean [--domain KEY] [--yes]` — reclaim segments whose publisher AND all subscribers are dead; dry-run by default, `--yes` unlinks (the never-unlink lifecycle's manual path).

The three bring-up faults and their signatures (M10-A3, each diagnosable from the CLI output alone):

| Symptom in `xmmsg` | Diagnosis |
|---|---|
| last-publish age rising, PUB-PID **live** | publisher paused/stuck — alive but not publishing |
| drop/overwrite count growing | consumer too slow (or stalled) for the declared QoS |
| PUB-PID marked **DEAD** | publisher crashed — segment persists, staleness verdicts raise at subscribers |

Scope, per R5: live state only — history, timelines, and post-mortem analysis belong to the telemetry plane, offline. `xmmsg` works with zero cooperation because every endpoint maintains its counters in the segment header unconditionally; the observer never takes a lock and cannot perturb the observed pair's latency (M10-A4/A5).

## 9. Status reference — every value, what to do

R2's contract: every failure mode a user can hit has a distinct, documented status. There are no exceptions anywhere on this surface; construction never throws — handles carry a queryable `status()` (D18). Publishing on a non-kOk handle or dereferencing a kNone sample is a contract violation (debug-assert).

### AdvertiseStatus (on `Publisher`/`Server` handles)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | normal wiring | the write end is live | proceed |
| `kOwnershipRefused` | a second `Advertise` on an exclusively-owned topic (or exclusive/shared mix; or a second `Serve`) | an accidentally duplicated node — the incumbent is untouched | treat as a launcher bug: log the topic, exit; restart re-advertises cleanly (recipe 7) |
| `kTypeMismatch` | the topic exists with a different payload schema hash (R6) | rebuild skew or a genuine type conflict — bytes were NOT exchanged | rebuild the older process; `xmmsg stat` shows both hashes (M11); for cross-process matching prefer `XMMSG_DESCRIBE`d payloads |
| `kUnsupportedReach` | the domain's backend is not compiled into this build, or the contract is a declared divergence on this reach (e.g. reliable queue / shared ownership / `Serve` on shm) | never a silent fallback — the reach honestly cannot carry this endpoint | check `Domain::Supports(...)` at wiring; enable the backend option, change reach, or change the QoS declaration |

### SubscribeStatus (on `Subscriber`/`Client` handles)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | normal wiring | the read end is live | proceed |
| `kTypeMismatch` | schema-hash mismatch against the topic's established type (R6) | as above — refusal, visible in introspection | as above |
| `kUnsupportedReach` | backend not compiled in / contract divergence on this reach | as above | as above |

### PublishStatus (from `Publish`/`PublishDerived`)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | best-effort always; reliable with queue space; latest-only always | the transport accepted the value (per-subscriber best-effort drops land in the subscribers' own counters, D8) | nothing |
| `kWouldBlock` | reliable + bounded queue full right now | nothing was enqueued; `Publish` never blocks internally; never occurs on latest-only | caller policy: retry / coalesce / shed — refusal is a NOW fact, capacity returns as the consumer drains (recipe 3) |

### LoanStatus (on a `Loan` handle, from `Publisher::Loan()`)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | normal | a writable slot in transport memory | construct in place, `Publish(std::move(loan))` |
| `kExhausted` | the loan pool declared at wiring time is empty | outstanding loans were neither published nor dropped | publish or scope-exit pending loans; accessing or publishing an exhausted loan is a contract violation |

### CallStatus (on a `Result`, from `Client::Call`)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | prompt server | typed response; deref valid only here | proceed |
| `kDeadlineExpired` | the deadline passed | server exists but was too slow — "overloaded"; the late reply is discarded by correlation and never surfaces on a later call (M5-A2) | retry with a longer deadline, or degrade |
| `kNoServer` | nobody serves the topic | fails fast (well before the deadline) — "crashed / not started", a different incident than timeout (M5-A3) | fix the launcher; gate on the readiness barrier first (recipe 6) |

### ReplyStatus (from `Server::Reply`)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kOk` | normal | reply correlated and delivered | nothing |
| `kExpired` | the caller's deadline already passed when you replied | the reply was discarded by correlation — observable, never an error path (D20) | optional: count it as a "server too slow" health signal |

### WaitStatus (from `Domain::WaitUntilMatched`)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kMatched` | all listed endpoints have ≥ 1 peer | the stack is wired | release motion |
| `kDeadlineExpired` | the deadline passed with ≥ 1 endpoint unmatched | "stack didn't come up" — never looks like success, never hangs (M14-A2) | interrogate `MatchedCount()` per endpoint to name the missing peer (recipe 6) |

### Freshness (on every `Sample`/`Request` — the take-side verdict, not an error)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kFresh` | newest value within the declared deadline (or no deadline declared) | act on it | proceed |
| `kStale` | value exceeded the declared deadline | "lost the producer" — value still accessible, `age()` is honest; the Fresh→Stale transition fired one `deadline_miss` event | your declared degradation policy (recipe 2) |
| `kNone` | never received (normal before matching; empty queue on `TryTake`; empty inbox on `TakeRequest`) | "never had one" — a different incident than kStale; dereference is a contract violation | wait, or check matching (recipe 6) |

### AgeClass (on every `Sample` — is the age a measurement or a hint)

| Value | When you see it | What it means | What to do |
|---|---|---|---|
| `kMeasured` | same-host reaches; declared-synced clock domains | `age()`/`origin_age()` are real measurements on the one monotonic clock (R8) | deadline verdicts apply |
| `kAdvisory` | inter-host without a `ClockDomain` declaration | ages are hints across unsynced clocks — an advisory age NEVER produces a kStale verdict (no confidently-wrong staleness) | declare `kPtpSynced`/`kNtpSynced` in the Zenoh config if the fleet really is synced |

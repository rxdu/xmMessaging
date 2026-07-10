// M6 — Reach transparency (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M6.
//
// Contract under test: the library's central promise. The scenario body
// below is written ONCE; only MakeDomain() differs per instantiation
// (M6-A1: assertion code byte-identical across reaches). This file also
// wishes the three Domain factories, the support-matrix query (R3), and
// the synced-clock declaration (R8) into existence.

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;

// -- The reach fixture: the ONLY code allowed to vary -----------------------
enum class Reach { kInProcess, kInterProcess, kInterHost };

msg::Domain MakeDomain(Reach reach) {
  switch (reach) {
    case Reach::kInProcess:
      return msg::Domain::InProcess();
    case Reach::kInterProcess:
      // Backend-specific config stays in the backend's own struct — the
      // portable API does not grow a union of every engine's options (R3).
      return msg::Domain::Iceoryx2({.service_name = "m6"});
    case Reach::kInterHost:
      // R8 at the wiring site: cross-host deadline semantics require the
      // application to DECLARE clock discipline. Without this field, ages
      // from remote publishers are advisory (see below); declaring it is
      // recorded in introspection — a post-hoc reader knows what the
      // numbers meant.
      return msg::Domain::Zenoh({.locator = "tcp/robot-nav:7447",
                                 .clock = msg::ClockDomain::kPtpSynced});
  }
}

// -- The scenario body: byte-identical on every reach ------------------------
void RunM1Coupling(msg::Domain& domain) {
  // M6-A4: reach requirements are compile-time facts. TrajectoryHead is
  // trivially copyable + standard-layout + explicitly padded, so it wires
  // on every reach. A type violating that must fail HERE, at Advertise<>,
  // with a static_assert naming the requirement — not at runtime on a
  // robot ("no serializer for T on the network reach" is a build error,
  // never a field discovery).
  auto pub = domain.Advertise<TrajectoryHead>(
      "m6.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m6.plan.head",
      {.history = msg::History::LatestOnly(), .deadline = kPlanDeadline});

  // M6-A6 (R3, divergence over emulation): before relying on a contract,
  // the application can ask. The answer comes from the per-reach support
  // matrix — the same table docs/design.md publishes — so "this transport
  // can't warm-start late joiners" is a wiring-time fact, not a silent
  // emulation or a field surprise.
  if (!domain.Supports(msg::Contract::kLateJoinWarmStart)) {
    LogStatedDivergence("late-join warm-start", domain.reach_name());
  }

  // ... M1 body verbatim: 10 Hz producer, 1 kHz consumer, the D1/D2
  // Sample/Freshness switch. Not repeated here — that is the point.
  RunM1ProducerConsumerPair(pub, sub);

  // M6-A5 (R8): the age a sample reports knows its own trustworthiness.
  // Same-host reaches and declared-synced domains: kMeasured. Inter-host
  // without a ClockDomain declaration: kAdvisory — and a kAdvisory age
  // never yields a deadline verdict (freshness stays kFresh/kNone; the
  // library must not issue confidently-wrong kStale from unsynced clocks).
  auto s = sub.TakeLatest();
  Assert(s.age_class() == msg::AgeClass::kMeasured);  // kPtpSynced declared above
}

int main() {
  // Behavioral versions instantiate per reach:
  //   in-process:    both ends in this binary          (P0b)
  //   inter-process: planner/control split, iceoryx2   (P1)
  //   inter-host:    two hosts / netns, Zenoh          (P2)
  // M6-A3: per-reach hop latency is recorded against the published
  // envelope for that reach — transparency of semantics, honesty of cost.
  auto domain = MakeDomain(kReachUnderTest);
  RunM1Coupling(domain);
  return 0;
}

// M14 — Stack cold start: order, readiness, isolation, ownership (P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M14.
//
// Contract under test: the composition-scale rules a real launcher lives
// by. Wiring is order-independent; readiness is one bounded verb;
// duplicate publishers are refused unless declared; two stacks on one
// host share nothing. This is the file a nav-stack launcher gets copied
// from, so it is written as one.

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;

int main() {
  // D17: the isolation key is the FIRST thing a Domain knows. Everything —
  // topics, shm segments, introspection entries — is namespaced under it.
  // Default derives from user + configured name; two stacks on one dev
  // machine collide on nothing unless they *choose* the same key.
  msg::Domain stack_a = msg::Domain::InProcess({.name = "stack_a"});

  // -- Order independence (M14-A1) ------------------------------------------
  // The launcher starts stages in ANY order (spawn order, scheduling,
  // restart races). Subscribe with no publisher in existence is silent
  // and normal — freshness is simply kNone until matching happens.
  auto est_sub = stack_a.Subscribe<StateEstimate>("m14.estimator.state", {});
  Assert(est_sub.MatchedCount() == 0);
  Assert(est_sub.TakeLatest().freshness() == msg::Freshness::kNone);

  auto est_pub = stack_a.Advertise<StateEstimate>(
      "m14.estimator.state", {.history = msg::History::LatestOnly()});
  // Matching is eventual and order-blind; after it completes, this wiring
  // is indistinguishable from the reverse order (behavioral test runs all
  // permutations of a three-stage pipeline and diffs the wired state).

  // -- Readiness: the e-stop-release gate (M14-A2) ---------------------------
  // D16: one bounded verb, built for launchers. Success exactly when every
  // listed endpoint has >=1 peer; a DISTINCT timeout status otherwise —
  // "stack didn't come up" must never look like success or hang forever.
  switch (stack_a.WaitUntilMatched({&est_sub, &est_pub},
                                   std::chrono::seconds(5))) {
    case msg::WaitStatus::kMatched:
      ReleaseEStop();
      break;
    case msg::WaitStatus::kDeadlineExpired:
      // Which endpoint is unmatched is queryable (MatchedCount per
      // endpoint) — the launcher reports WHO is missing, not just "no".
      AbortLaunch(ReportUnmatched({&est_sub, &est_pub}));
      break;
  }

  // -- Ownership: exclusive by default (M14-A3) ------------------------------
  // A duplicated estimator process is a config error, not a coin toss.
  // The second Advertise on an exclusive topic is REFUSED — distinct
  // status, introspection-visible, first publisher untouched.
  auto dup = stack_a.Advertise<StateEstimate>("m14.estimator.state", {});
  Assert(dup.status() == msg::AdvertiseStatus::kOwnershipRefused);

  // The deliberate case is a declaration, made by BOTH publishers:
  // planner + recovery planner on one setpoint topic, last-writer-wins
  // by publish stamp — deterministic under any interleaving.
  auto primary = stack_a.Advertise<Setpoint>(
      "m14.control.setpoint", {.ownership = msg::Ownership::kShared});
  auto recovery = stack_a.Advertise<Setpoint>(
      "m14.control.setpoint", {.ownership = msg::Ownership::kShared});

  // -- Isolation: the second stack on the same host (M14-A4) -----------------
  msg::Domain stack_b = msg::Domain::InProcess({.name = "stack_b"});
  auto b_sub = stack_b.Subscribe<StateEstimate>("m14.estimator.state", {});
  est_pub.Publish(SomeEstimate());
  // Same topic string, different domain key: no visibility, ever.
  Assert(b_sub.MatchedCount() == 0);
  Assert(b_sub.TakeLatest().freshness() == msg::Freshness::kNone);
  // (Behavioral inter-process version also asserts the negative at the
  // introspection layer: stack_b's segment lists no stack_a entries.)

  // -- Liveness tracking through restart (M14-A5) ----------------------------
  // MatchedCount() follows peers through death and rejoin — the M4 story
  // observable from the surviving side's wiring, not only from staleness.
  KillAndRestartPublisher(est_pub);
  Assert(EventuallyEquals([&] { return est_sub.MatchedCount(); }, 1));

  return 0;
}

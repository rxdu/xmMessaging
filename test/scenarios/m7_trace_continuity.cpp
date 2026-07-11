// M7 — Trace continuity across the hop (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M7.
//
// Contract under test: the envelope. "The planning stall and the motor
// fault on one timeline" must be a property of the transport, not
// per-application discipline. The asymmetry wished here is deliberate:
//   - publish side: context capture is AUTOMATIC (whatever trace is
//     active on the calling thread rides in the envelope, zero plumbing);
//   - take side: adoption is EXPLICIT (one RAII scope) — the consumer
//     may have its own trace, and silently reparenting it would be the
//     cross-contamination M7-A2 forbids.

#include "xmbase/telemetry/telemetry.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
namespace tel = xmotion::telemetry;

int main() {
  msg::Domain domain = msg::Domain::InProcess();
  auto pub = domain.Advertise<TrajectoryHead>("m7.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>("m7.plan.head", {.history = msg::History::LatestOnly()});

  // -- Producer: an ordinary instrumented planning iteration ----------------
  std::thread planner([&] {
    while (running) {
      tel::NewTrace();                       // iteration root, plain telemetry API
      XM_SPAN("m7.plan.iteration");
      auto loan = pub.Loan();
      ComputePlan(*loan);
      // The wish: nothing here mentions context. Publish() snapshots the
      // calling thread's active telemetry context into the envelope's
      // fixed-size header field (the xmBase Inject bytes) alongside the
      // stamp. If no trace is active, the field is the null context —
      // valid, never garbage (M7-A3: fixed size either way).
      pub.Publish(std::move(loan));
      SleepUntilNextPlan();
    }
  });

  // -- Consumer: adopts the producer's context, once, visibly ---------------
  std::thread control([&] {
    while (running) {
      auto plan = sub.TakeLatest();
      if (plan.freshness() == msg::Freshness::kFresh) {
        // The wish: adoption is a scope, not a mode. Inside it, spans are
        // children of the producer's trace (M7-A1: one TraceId, correct
        // causal links across the hop); at scope exit the thread's prior
        // context is restored exactly — a control loop that also runs its
        // own trace keeps it uncontaminated (M7-A2, the telemetry S2-A5
        // pool-thread discipline applied across the transport).
        tel::ContextScope hop(plan.context());
        XM_SPAN("m7.ctrl.consume_plan");
        TrackPlan(*plan);
      }
      SpinUntilNextCycle();
    }
  });

  // Behavioral assertions (P0b in-proc, P1 across processes):
  //  - producer iteration span and consumer consume_plan span share one
  //    TraceId with parent/child linkage across the hop;
  //  - two interleaved traces through the same topic never cross (a
  //    latest-only overwrite discards the overwritten value's context with
  //    it — the envelope travels with the value, never ambiently);
  //  - sizeof(envelope) is a compile-time constant: context bytes + stamp,
  //    accounted in the loan budget, identical with and without an active
  //    trace.
  RunFor(std::chrono::seconds(5));
  running = false;
  planner.join();
  control.join();
  return 0;
}

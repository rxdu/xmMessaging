// M13 — Pipeline lineage & system evaluation (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M13.
//
// Contract under test: R11. A three-stage fake navigation pipeline
// (sensor -> estimator -> controller) whose end-to-end behavior is
// quantitatively evaluable from what the transport records — zero custom
// instrumentation. The load-bearing verb is PublishDerived (D14): it is
// how "how old is the information" survives a hop while "how old is the
// value" resets.

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;

struct PoseSample   { double x, y, yaw; };            // sensor output
struct StateEstimate { double x, y, yaw, vx, vy; };   // estimator output

int main() {
  msg::Domain domain = msg::Domain::InProcess();

  // -- Stage 1: sensor fake, 100 Hz — the lineage ORIGIN --------------------
  auto pose_pub = domain.Advertise<PoseSample>(
      "m13.sensor.pose", {.history = msg::History::LatestOnly()});
  std::thread sensor([&] {
    while (running) {
      // Plain Publish: first hop. origin stamp == publish stamp, hops == 0
      // (M13-A2). Nothing to remember at the origin — lineage starts free.
      pose_pub.Publish(ReadFakePose());
      SleepHz(100);
    }
  });

  // -- Stage 2: estimator fake — the lineage-PRESERVING hop -----------------
  auto pose_sub = domain.Subscribe<PoseSample>("m13.sensor.pose", {});
  auto est_pub = domain.Advertise<StateEstimate>(
      "m13.estimator.state", {.history = msg::History::LatestOnly()});
  std::thread estimator([&] {
    while (running) {
      auto pose = pose_sub.TakeLatest();
      if (pose.freshness() != msg::Freshness::kNone) {
        auto loan = est_pub.Loan();
        *loan = Fuse(*pose);
        // The wish: deriving is one verb, not bookkeeping. PublishDerived
        // stamps NOW as the publish time but carries forward the origin
        // stamp of the consumed input (oldest, if several) and increments
        // the hop count. A component that fuses N inputs passes them all;
        // the envelope keeps the most pessimistic origin — information is
        // only as fresh as its stalest ingredient.
        est_pub.PublishDerived(std::move(loan), pose);
      }
      MaybeInjectStall();  // fault case: 300 ms pause (M13-A1)
      SleepHz(50);
    }
  });

  // -- Stage 3: controller fake, 1 kHz — where the two ages diverge ---------
  auto est_sub = domain.Subscribe<StateEstimate>(
      "m13.estimator.state",
      {.history = msg::History::LatestOnly(), .deadline = kStateDeadline});
  std::thread controller([&] {
    while (running) {
      auto est = est_sub.TakeLatest();
      if (est.freshness() == msg::Freshness::kFresh) {
        // M13-A1, the scenario's reason to exist: during the estimator
        // stall, est.age() stays small (the estimator keeps republishing
        // its last fusion) while est.origin_age() grows — a fresh VALUE
        // built from stale INFORMATION. The controller can gate on the
        // one that matters for safety, per-cycle, with no plumbing:
        if (est.origin_age() > kMaxInformationAge) {
          EnterDegradedTracking();   // robot policy; the API's job was
        }                            // only to make the distinction free
        Assert(est.hop_count() == 1);  // sensor(0) -> estimator(+1)
      }
      SpinUntilNextCycle();
    }
  });

  RunFor(std::chrono::seconds(10));
  running = false;
  JoinAll(sensor, estimator, controller);

  // -- The R11 payoff, asserted from records alone --------------------------
  // With the telemetry SDK bound to a capture sink, the behavioral test
  // computes — WITHOUT any scenario-side instrumentation:
  //  - end-to-end sensor->controller latency distribution from envelope
  //    fields in the captured records, matching ground truth (M13-A3);
  //  - presence + units + labels of every standard-schema instrument for
  //    all six endpoints (M13-A4): messaging.pub.*/messaging.sub.* were
  //    emitted by the library, not by this file;
  //  - one-timeline ordering: no transport hop precedes the publish span
  //    that caused it (M13-A5) — messaging stamps and telemetry records
  //    share the xmBase clock by construction.
  return 0;
}

// M1 — Planner → control at rate (wish-code, P0.0)
//
// NOT COMPILED. This file is the API specification: the wiring code we wish
// an application could write for the flagship coupling (10 Hz planner,
// 1 kHz control loop, latest-only). Every name below is a proposal; deltas
// discovered while writing this go to docs/scenarios.md "API deltas".
//
// Contract under test: LatestMailbox (design.md) — depth-1 overwrite,
// stamped values, consumer-side deadline, counted overwrites, explicit
// everything. Reach-parametric: the same code must wire in-process (P0b)
// and across processes via iceoryx2 (P1).

#include "xmmessaging/messaging.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace msg = xmotion::messaging;

// Scenario-local POD shaped like the real planning vocabulary (which lives
// in xmNavigation's types tier — components own payload meaning).
struct TrajectoryHead {
  uint64_t plan_id;
  double x[8];
  double y[8];
  double v[8];
  double t_offset[8];
  uint32_t checksum;  // M1-A2 tear detection
};
static_assert(std::is_trivially_copyable_v<TrajectoryHead>);

constexpr auto kPlanDeadline = std::chrono::milliseconds(250);

int main() {
  // -- Wiring (application-owned, done once, before the loops start) -------
  // A Domain is the application's messaging session: it owns the reach
  // configuration and every endpoint created from it. In-process reach
  // needs no backend and no configuration.
  msg::Domain domain = msg::Domain::InProcess();

  auto pub = domain.Advertise<TrajectoryHead>(
      "m1.plan.head", {.history = msg::History::LatestOnly()});

  auto sub = domain.Subscribe<TrajectoryHead>(
      "m1.plan.head",
      {.history = msg::History::LatestOnly(), .deadline = kPlanDeadline});

  std::atomic<bool> running{true};

  // -- Producer: fake planner at 10 Hz -------------------------------------
  std::thread planner([&] {
    uint64_t id = 0;
    while (running) {
      // Zero-copy publication: construct in place, publish moves ownership.
      // On latest-only, Publish never blocks and never refuses (contract
      // M1-A1) — the returned status exists for uniformity and reports
      // whether an unread value was overwritten.
      auto loan = pub.Loan();
      FillFakePlan(*loan, ++id);
      msg::PublishStatus s = pub.Publish(std::move(loan));
      (void)s;  // overwrite info also lands in messaging.* metrics (A5)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  // -- Consumer: control loop at 1 kHz --------------------------------------
  // Call-site shape decided 2026-07-10 (variant A of three candidates; see
  // API deltas D1-D5 in docs/scenarios.md for what this commits the API to
  // and which alternatives were declined).
  std::thread control([&] {
    Setpoint setpoint = SafeStop();
    while (running) {
      // TakeLatest() is wait-free and allocation-free (R7). It always
      // returns a Sample<TrajectoryHead>: value + monotonic stamp + a
      // tri-state freshness verdict judged by the library against the
      // deadline declared at wiring (R8) — emptiness and staleness are
      // states of the sample, not different return types.
      auto plan = sub.TakeLatest();
      switch (plan.freshness()) {
        case msg::Freshness::kFresh:
          // Newest value, within deadline. plan.age() available for
          // prediction/interpolation along the plan.
          setpoint = TrackPlan(*plan, plan.age());
          break;
        case msg::Freshness::kStale:
          // A value exists but exceeded the 250 ms deadline. The policy
          // line is robot-specific and deliberately lives here in app
          // code — the API's job was only to make this state impossible
          // to confuse with the other two. The Fresh->Stale transition
          // also fires a messaging.* deadline-miss event (M1-A3).
          setpoint = DecayToStop(setpoint);
          break;
        case msg::Freshness::kNone:
          // Nothing ever received (startup / planner never came up).
          // Distinct from kStale: "never had a plan" and "lost the
          // planner" are different incidents with different responses.
          setpoint = SafeStop();
          break;
      }
      Apply(setpoint);
      SpinUntilNextCycle();  // 1 kHz pacing stays with the loop, never the transport
    }
  });

  RunFor(std::chrono::seconds(10));
  running = false;
  planner.join();
  control.join();

  // -- Reconciliation (M1-A1, A5): ground truth vs library metrics ---------
  // published == consumer_seen_unique + overwritten_unread, exactly;
  // messaging.m1.plan.head.overwrite_count agrees with ground truth.
  return 0;
}

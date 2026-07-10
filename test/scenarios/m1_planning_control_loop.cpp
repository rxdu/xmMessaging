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
  std::thread control([&] {
    while (running) {
      // =======================================================================
      // TODO(user contribution): the consume call-site — the single most
      // API-shaping decision in this suite. Write the ~5-10 lines you WISH
      // the 1 kHz loop could be, answering three questions the design doc
      // deliberately leaves to wish-code:
      //
      //   1. Take shape: does the loop poll a snapshot
      //      (`auto v = sub.TakeLatest()` returning an optional-like stamped
      //      value), hold a zero-copy view that must be released, or block
      //      with a bound (`sub.WaitFor(cycle_budget)`)? Remember M1-A4:
      //      no allocation, no unbounded blocking on this path.
      //   2. Staleness surface: given deadline=250ms was declared at wiring,
      //      how does this call-site *read* it — a flag on the taken value
      //      (`v.deadline_missed()`), a distinct return state, or an age you
      //      compare yourself (`v.age() > kPlanDeadline`)?
      //   3. Degradation policy: on stale/missing plan, what does the loop
      //      do — hold last setpoint, command zero, or escalate? (This line
      //      is robot policy, but WHERE it hooks in shapes the API.)
      //
      // The M1 acceptance criteria (A1-A4) must be assertable against
      // whatever you write here.
      // =======================================================================

      SpinUntilNextCycle();  // 1 kHz pacing, fake
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

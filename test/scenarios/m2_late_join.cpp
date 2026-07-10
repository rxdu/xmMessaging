// M2 — Mid-run subscriber join (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M2 for the
// acceptance criteria this call-site must make assertable.
//
// Contract under test: latest-only warm-start. A subscriber created
// mid-run on a latest-only topic starts from the current value — with its
// ORIGINAL stamp, so age never lies — and joining/leaving never perturbs
// the incumbent subscriber.

#include "xmmessaging/messaging.hpp"

#include <thread>

namespace msg = xmotion::messaging;

struct RobotState {
  double x, y, yaw;
  uint64_t seq;
};

int main() {
  msg::Domain domain = msg::Domain::InProcess();

  // Deliberately slow topic (1 Hz): a late joiner that had to wait for the
  // next publish would stare at nothing for up to a second — the exact
  // failure this scenario exists to forbid.
  auto pub = domain.Advertise<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  auto incumbent = domain.Subscribe<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  pub.Publish({.x = 1.0, .y = 2.0, .yaw = 0.5, .seq = 1});  // t0

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // The wish: joining mid-run is the SAME call as joining at wiring time.
  // No epoch, no "transient-local" QoS knob, no publisher-side opt-in:
  // on a latest-only topic the slot exists, so a new subscriber reads it.
  // Warm-start is intrinsic to latest-only, not a fifth QoS knob.
  auto late = domain.Subscribe<RobotState>(
      "m2.robot.state", {.history = msg::History::LatestOnly()});

  auto first = late.TakeLatest();
  // M2-A1: kFresh immediately (no deadline declared -> stale never applies),
  // value is the t0 publish, and — the part that keeps R8 honest — the
  // stamp is t0, so first.age() reports ~500 ms, not ~0. A warm start must
  // never make old data look new.
  Assert(first.freshness() == msg::Freshness::kFresh);
  Assert((*first).seq == 1);
  Assert(first.age() >= std::chrono::milliseconds(500));

  // M2-A3: each subscriber holds an independent LatestMailbox — the late
  // joiner's take must not consume or disturb the incumbent's slot.
  auto inc = incumbent.TakeLatest();
  Assert(inc.freshness() == msg::Freshness::kFresh && (*inc).seq == 1);

  // M2-A2: subscriber lifetime is undramatic — destroy the late joiner
  // mid-stream; the incumbent sees every subsequent publish exactly as if
  // nothing happened. (Asserted over a publish burst in the behavioral
  // version; wish here is simply that teardown is scope exit, no Unsubscribe
  // ceremony.)
  { auto transient = domain.Subscribe<RobotState>("m2.robot.state", {}); }
  pub.Publish({.x = 1.1, .y = 2.0, .yaw = 0.5, .seq = 2});
  Assert((*incumbent.TakeLatest()).seq == 2);

  return 0;
}

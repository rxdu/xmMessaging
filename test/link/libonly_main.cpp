/*
 * test/link/libonly_main.cpp — M8: the lib-only build proof.
 *
 * A minimal M1-shaped in-process pub/sub pair, built as a real executable
 * linking ONLY xmotion::xmmessaging (+ xmBase transitively) — deliberately
 * no gtest, so the binary's dependency closure is exactly what a lib-only
 * application would carry. CTest runs it (M8-A1) and a companion script
 * asserts its `ldd` closure contains no iceoryx2/zenoh references (M8-A3).
 *
 * It also asserts M8-A2 at runtime: requesting a reach whose backend is not
 * compiled into this build yields an explicit kUnsupportedReach on every
 * endpoint — never a silent in-process fallback (a robot that silently
 * isn't distributed is a field incident).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdint>
#include <cstdio>

#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

// M1-shaped POD payload: a small trajectory head.
struct PlanHead {
  std::uint64_t plan_id;
  double x[4];
  double v[4];
};

int Fail(const char* what) {
  std::fprintf(stderr, "m8_libonly: FAIL — %s\n", what);
  return 1;
}

// M8-A2: a Domain minted by an unavailable-backend factory must carry the
// unsupported-reach status on every endpoint it wires, and Supports() must
// answer no to every portable contract.
int CheckUnsupportedReach(msg::Domain domain, const char* factory) {
  auto pub = domain.Advertise<PlanHead>("m8.unsupported.head");
  auto sub = domain.Subscribe<PlanHead>("m8.unsupported.head");
  if (pub.status() != msg::AdvertiseStatus::kUnsupportedReach) {
    std::fprintf(stderr, "m8_libonly: FAIL — %s Advertise did not carry "
                 "kUnsupportedReach (M8-A2)\n", factory);
    return 1;
  }
  if (sub.status() != msg::SubscribeStatus::kUnsupportedReach) {
    std::fprintf(stderr, "m8_libonly: FAIL — %s Subscribe did not carry "
                 "kUnsupportedReach (M8-A2)\n", factory);
    return 1;
  }
  if (domain.Supports(msg::Contract::kLatestOnly)) {
    std::fprintf(stderr, "m8_libonly: FAIL — %s Supports() claims contracts "
                 "on an unavailable reach (M8-A2/M6-A6)\n", factory);
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  // --- M8-A1: the in-process reach works with zero transport deps ----------
  auto domain = msg::Domain::InProcess({.name = "m8_libonly"});
  auto pub = domain.Advertise<PlanHead>(
      "m8.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<PlanHead>(
      "m8.plan.head", {.history = msg::History::LatestOnly()});
  if (pub.status() != msg::AdvertiseStatus::kOk) {
    return Fail("Advertise not kOk on the in-process reach");
  }
  if (sub.status() != msg::SubscribeStatus::kOk) {
    return Fail("Subscribe not kOk on the in-process reach");
  }
  if (domain.WaitUntilMatched({&pub, &sub}, 1s) != msg::WaitStatus::kMatched) {
    return Fail("pub/sub pair did not match (D16 barrier)");
  }

  PlanHead head{};
  head.plan_id = 42;
  for (int i = 0; i < 4; ++i) {
    head.x[i] = 1.0 * i;
    head.v[i] = 0.5 * i;
  }
  if (pub.Publish(head) != msg::PublishStatus::kOk) {
    return Fail("Publish not kOk (latest-only never refuses for capacity)");
  }
  auto sample = sub.TakeLatest();
  if (sample.freshness() != msg::Freshness::kFresh) {
    return Fail("TakeLatest not kFresh after one publish");
  }
  if (sample->plan_id != 42 || sample->x[3] != 3.0) {
    return Fail("taken value does not match the published one");
  }

  // --- M8-A2: unavailable reaches fail explicitly, never fall back ---------
  // The two external-dependency backends (the ones the ldd gate excludes).
  // PosixShm is dependency-free and lands at P1b; it is not asserted here so
  // this gate stays valid unmodified when that backend arrives.
  if (CheckUnsupportedReach(msg::Domain::Iceoryx2({.service_name = "m8"}),
                            "Iceoryx2") != 0 ||
      CheckUnsupportedReach(msg::Domain::Zenoh({.locator = "tcp/host:7447"}),
                            "Zenoh") != 0) {
    return 1;
  }

  std::printf("m8_libonly: PASS — in-process pub/take round-trip on a "
              "lib-only link; unavailable reaches explicit (M8-A1/A2)\n");
  return 0;
}

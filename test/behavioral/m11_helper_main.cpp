/*
 * m11_helper_main.cpp — the skewed-build subscriber for M11. One source,
 * compiled FOUR times with -DXMMSG_M11_VARIANT=<n> (m11_payload.hpp): each
 * binary is genuinely a separate build carrying its own idea of M11Plan's
 * layout — the rebuild-skew incident, reproduced honestly instead of
 * simulated with hand-fed hashes.
 *
 * Usage: m11_helper_v<n> <expect_match|expect_refusal> <domain>
 *                        <main_topic> <side_topic>
 *
 * The helper subscribes to <main_topic> with ITS OWN M11Plan variant and
 * verifies the wiring outcome against the expectation; then — regardless
 * of that outcome — subscribes to <side_topic> (the shared ShmTestPlan
 * type) and requires a fresh value, proving the refusal is LOCAL to the
 * mismatched topic (M11-A5: other topics between the same two processes
 * keep flowing).
 *
 * Exit codes:
 *   0   expectation met on the main topic AND the side topic flowed
 *   10  expected refusal, but the subscribe was accepted (R6 hole!)
 *   11  expected match, but the subscribe was refused
 *   12  main-topic subscribe returned an unexpected third status
 *   13  expected match, but no fresh value ever arrived on the main topic
 *   14  side-topic subscribe refused (A5 violated at wiring)
 *   15  side topic never delivered a fresh value (A5 violated in flow)
 *   64  usage
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#define XMMSG_SHM_HELPER_BUILD
#include "m11_payload.hpp"
#include "shm_test_support.hpp"

#if !defined(XMMESSAGING_HAS_POSIX_SHM)
int main() { return 64; }
#else

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

// Bounded fresh-value wait (the helper owns its cadence, D5).
template <typename T>
bool TakeFreshBounded(msg::Subscriber<T>& sub, msg::Duration bound) {
  const auto deadline = xmotion::Now() + bound;
  while (xmotion::Now() < deadline) {
    if (sub.TakeLatest().freshness() == msg::Freshness::kFresh) {
      return true;
    }
    ::usleep(2000);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    return 64;
  }
  const std::string expectation = argv[1];
  const std::string domain_name = argv[2];
  const std::string main_topic = argv[3];
  const std::string side_topic = argv[4];
  const bool expect_match = expectation == "expect_match";
  if (!expect_match && expectation != "expect_refusal") {
    return 64;
  }

  auto domain = msg::Domain::PosixShm({.name = domain_name});

  // The skewed subscribe: THIS binary's M11Plan against whatever hash the
  // advertising process established in the segment (R6, M11-A1/A2).
  auto skewed = domain.Subscribe<M11Plan>(
      main_topic, {.history = msg::History::LatestOnly()});
  switch (skewed.status()) {
    case msg::SubscribeStatus::kOk:
      if (!expect_match) {
        return 10;  // a skewed layout was accepted — the R6 hole
      }
      // A4 control case: the match must also actually flow.
      if (!TakeFreshBounded(skewed, 5s)) {
        return 13;
      }
      break;
    case msg::SubscribeStatus::kTypeMismatch:
      if (expect_match) {
        return 11;  // identical layout refused — hash not build-stable
      }
      break;  // the expected refusal (A1): distinct status, no bytes taken
    default:
      return 12;
  }

  // A5: the refusal is local — a second topic between the SAME two
  // processes must flow while the first stays refused.
  auto side = domain.Subscribe<ShmTestPlan>(
      side_topic, {.history = msg::History::LatestOnly()});
  if (side.status() != msg::SubscribeStatus::kOk) {
    return 14;
  }
  if (!TakeFreshBounded(side, 5s)) {
    return 15;
  }
  return 0;
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

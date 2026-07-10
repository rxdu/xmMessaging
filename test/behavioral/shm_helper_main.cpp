/*
 * shm_helper_main.cpp — the child-process side of the POSIX-shm (P1b)
 * cross-process behavioral legs. One purpose-built, single-threaded binary
 * with argv-selected roles (fork+exec from the tests — see
 * shm_test_support.hpp for why this beats forking the gtest binary).
 *
 * Roles (all: argv[1]=role argv[2]=domain_name argv[3]=topic):
 *   publish_stream <start_id> <count> <period_us>
 *       Advertise latest-only, wait for >= 1 subscriber (bounded), publish
 *       ids start_id..start_id+count-1 at the given period. The M1/M4
 *       producer; M4 SIGKILLs it mid-stream.
 *   publish_once <id>
 *       Advertise latest-only, publish one value, exit — no matching wait
 *       (the M2 warm-start producer: the value must outlive this process).
 *   flood_queue <count>
 *       Advertise (best-effort default), wait for >= 1 subscriber, publish
 *       <count> values as fast as possible (the M3 flood producer).
 *   publish_pause <n> <period_us> <pause_ms> <m>
 *       Advertise latest-only, wait for >= 1 subscriber, publish n values
 *       at the period, go silent for pause_ms WHILE STAYING ALIVE, then
 *       publish m more (the M10-A3 "paused publisher" fault: rising
 *       last-publish age with a live pid — distinct from death).
 *
 * Exit codes: 0 ok; 2 Advertise refused; 3 match wait expired; 64 usage.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#define XMMSG_SHM_HELPER_BUILD
#include "shm_test_support.hpp"

#if !defined(XMMESSAGING_HAS_POSIX_SHM)
int main() { return 64; }
#else

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc < 4) {
    return 64;
  }
  const std::string role = argv[1];
  const std::string domain_name = argv[2];
  const std::string topic = argv[3];

  auto domain = msg::Domain::PosixShm({.name = domain_name});

  if (role == "publish_stream" && argc == 7) {
    const std::uint64_t start_id = std::strtoull(argv[4], nullptr, 10);
    const std::uint64_t count = std::strtoull(argv[5], nullptr, 10);
    const long period_us = std::strtol(argv[6], nullptr, 10);
    auto pub = domain.Advertise<ShmTestPlan>(
        topic, {.history = msg::History::LatestOnly()});
    if (pub.status() != msg::AdvertiseStatus::kOk) {
      return 2;  // M4-A2 would fail here if reclaim were broken
    }
    if (domain.WaitUntilMatched({&pub}, 5s) != msg::WaitStatus::kMatched) {
      return 3;
    }
    ShmTestPlan plan{};
    for (std::uint64_t i = 0; i < count; ++i) {
      FillShmPlan(plan, start_id + i);
      pub.Publish(plan);
      if (period_us > 0) {
        ::usleep(static_cast<useconds_t>(period_us));
      }
    }
    return 0;
  }

  if (role == "publish_once" && argc == 5) {
    const std::uint64_t id = std::strtoull(argv[4], nullptr, 10);
    auto pub = domain.Advertise<ShmTestPlan>(
        topic, {.history = msg::History::LatestOnly()});
    if (pub.status() != msg::AdvertiseStatus::kOk) {
      return 2;
    }
    ShmTestPlan plan{};
    FillShmPlan(plan, id);
    pub.Publish(plan);
    return 0;  // exits immediately: the master slot must warm-start M2
  }

  if (role == "publish_pause" && argc == 8) {
    const std::uint64_t n = std::strtoull(argv[4], nullptr, 10);
    const long period_us = std::strtol(argv[5], nullptr, 10);
    const long pause_ms = std::strtol(argv[6], nullptr, 10);
    const std::uint64_t m = std::strtoull(argv[7], nullptr, 10);
    auto pub = domain.Advertise<ShmTestPlan>(
        topic, {.history = msg::History::LatestOnly()});
    if (pub.status() != msg::AdvertiseStatus::kOk) {
      return 2;
    }
    if (domain.WaitUntilMatched({&pub}, 5s) != msg::WaitStatus::kMatched) {
      return 3;
    }
    ShmTestPlan plan{};
    for (std::uint64_t i = 0; i < n; ++i) {
      FillShmPlan(plan, i + 1);
      pub.Publish(plan);
      if (period_us > 0) {
        ::usleep(static_cast<useconds_t>(period_us));
      }
    }
    ::usleep(static_cast<useconds_t>(pause_ms) * 1000u);  // the fault
    for (std::uint64_t i = 0; i < m; ++i) {
      FillShmPlan(plan, n + i + 1);
      pub.Publish(plan);
      if (period_us > 0) {
        ::usleep(static_cast<useconds_t>(period_us));
      }
    }
    return 0;
  }

  if (role == "flood_queue" && argc == 5) {
    const std::uint64_t count = std::strtoull(argv[4], nullptr, 10);
    auto pub = domain.Advertise<ShmTestPlan>(topic, {});  // best-effort (D8)
    if (pub.status() != msg::AdvertiseStatus::kOk) {
      return 2;
    }
    if (domain.WaitUntilMatched({&pub}, 5s) != msg::WaitStatus::kMatched) {
      return 3;
    }
    ShmTestPlan plan{};
    for (std::uint64_t i = 0; i < count; ++i) {
      FillShmPlan(plan, i + 1);
      pub.Publish(plan);  // best-effort: always kOk, drops are counted (M3)
    }
    return 0;
  }

  return 64;
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

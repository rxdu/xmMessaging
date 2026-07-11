/*
 * scenario_support.hpp
 *
 * P0a compile-only support for the wish-code scenario suite. The wish-code
 * files (docs/scenarios.md, P0.0) are the API specification and compile
 * UNMODIFIED: this header is force-included ahead of each scenario TU
 * (-include, see CMakeLists.txt) and declares the scenario-side helper
 * pseudo-functions, fakes, and constants the wish-code calls. Everything
 * here is DECLARED, not defined — the suite builds as an OBJECT library
 * (compile-only, no linking); definitions arrive with the behavioral tests
 * at P0b.
 *
 * Two scenario-local types are shared across TUs (TrajectoryHead from M1,
 * StateEstimate from M13). The defining TU sets its XMMSG_SCENARIO_DEFINES_*
 * macro (per-source compile definition in CMakeLists.txt) so this header
 * only forward-declares there and defines the identical layout everywhere
 * else.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <thread>

#include "xmbase/telemetry/telemetry.hpp"
#include "xmmsg/messaging.hpp"

// D13 names the take-side adoption RAII `tel::ContextScope`; xmBase spells
// it ContextGuard (context.hpp). Alias here so M7 compiles as wished —
// open item: reconcile the spelling (add the alias to xmBase, or amend D13).
namespace xmotion {
namespace telemetry {
using ContextScope = ContextGuard;
}  // namespace telemetry
}  // namespace xmotion

// ---- shared run state -------------------------------------------------------

// Scenario run flag (M5/M7/M13 reference it as a global; M1 shadows it with
// a local). Declared only — no definitions in P0a.
extern std::atomic<bool> running;

// Ground-truth assertion used by the wish-code (gtest arrives at P0b).
void Assert(bool condition);

// ---- pacing / lifecycle helpers ---------------------------------------------

void RunFor(xmotion::Duration duration);
void SpinUntilNextCycle();     // 1 kHz pacing owned by the loop (D5)
void SleepHz(int rate_hz);     // producer pacing
void SleepUntilNextPlan();     // 10 Hz planner pacing (M7)

template <typename... Threads>
void JoinAll(Threads&... threads);

// ---- shared payload fakes (M1 vocabulary) -----------------------------------

struct TrajectoryHead;
#if !defined(XMMSG_SCENARIO_DEFINES_TRAJECTORY_HEAD)
// Identical layout to M1's scenario-local definition (the defining TU).
struct TrajectoryHead {
  uint64_t plan_id;
  double x[8];
  double y[8];
  double v[8];
  double t_offset[8];
  uint32_t checksum;  // M1-A2 tear detection
};
inline constexpr auto kPlanDeadline = std::chrono::milliseconds(250);
#endif

struct Setpoint {
  double v = 0.0;
  double w = 0.0;
};

Setpoint SafeStop();
Setpoint DecayToStop(const Setpoint& current);
void Apply(const Setpoint& setpoint);
Setpoint TrackPlan(const TrajectoryHead& plan, xmotion::Duration age);  // M1
void TrackPlan(const TrajectoryHead& plan);                             // M7
void FillFakePlan(TrajectoryHead& plan, std::uint64_t plan_id);
void ComputePlan(TrajectoryHead& plan);

// ---- M3: drain/accounting helpers -------------------------------------------

template <typename Sub>
void DrainAll(Sub& subscriber);

template <typename Sub>
std::uint64_t Seen(const Sub& subscriber);

// ---- M5: fake parameter server ----------------------------------------------

inline constexpr std::uint32_t kModeTurtle = 2;

double LookupKp(std::uint32_t mode);

template <typename Gains>
void ApplyGains(const Gains& gains);

void AbortModeSwitch(std::string_view reason);

// D10's bounded-park verb in the free-function shape the wish-code uses
// (the API also declares it as Server::WaitForWorkOrShutdown(max_park) —
// open item: pick one spelling before P0b).
template <typename ServerT>
void WaitForWorkOrShutdown(ServerT& server);

// ---- M6: reach fixture ------------------------------------------------------

enum class Reach;  // completed by M6's scenario-local definition
extern const Reach kReachUnderTest;

void LogStatedDivergence(std::string_view contract, std::string_view reach);

template <typename Pub, typename Sub>
void RunM1ProducerConsumerPair(Pub& pub, Sub& sub);

// ---- M13: three-stage pipeline fakes ----------------------------------------

struct PoseSample;  // defined by M13 (scenario-local)
struct StateEstimate;
#if !defined(XMMSG_SCENARIO_DEFINES_STATE_ESTIMATE)
// Identical layout to M13's scenario-local definition (the defining TU).
struct StateEstimate {
  double x, y, yaw, vx, vy;
};
#endif

PoseSample ReadFakePose();
StateEstimate Fuse(const PoseSample& pose);
void MaybeInjectStall();
void EnterDegradedTracking();

inline constexpr auto kStateDeadline = std::chrono::milliseconds(100);
inline constexpr auto kMaxInformationAge = std::chrono::milliseconds(150);

// ---- M14: launcher fakes ----------------------------------------------------

void ReleaseEStop();
void AbortLaunch(std::string_view report);
std::string_view ReportUnmatched(
    std::initializer_list<const xmotion::messaging::Endpoint*> endpoints);
StateEstimate SomeEstimate();

template <typename Pub>
void KillAndRestartPublisher(Pub& publisher);

// Defined inline (unlike the rest of this header): M14 instantiates it with
// a lambda, and a template instantiated with a local type cannot be defined
// in another TU — GCC rejects the declared-only form outright. The real
// bounded retry loop arrives at P0b; this stand-in keeps P0a compile-only.
template <typename Getter, typename Value>
inline bool EventuallyEquals(Getter&& getter, Value expected) {
  return static_cast<Value>(getter()) == expected;
}

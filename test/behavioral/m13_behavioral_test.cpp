/*
 * m13_behavioral_test.cpp — M13: pipeline lineage & system evaluation (P0b,
 * in-process leg).
 *
 * A three-stage fake navigation pipeline — sensor (origin) -> estimator
 * (PublishDerived, lineage-preserving) -> controller (PublishDerived) ->
 * recorder — six endpoints on three topics. Asserts the M13 acceptance
 * criteria from docs/scenarios.md:
 *   A1 — origin_age vs age diverge under an injected estimator stall: a
 *        fresh VALUE built from stale INFORMATION is distinguishable at the
 *        call site, with zero plumbing (D14).
 *   A2 — hop count increments per stage; a first-hop publish has
 *        origin == stamp and hops == 0.
 *   A3 — end-to-end latency is computable from the envelope stamps captured
 *        at the consumer alone, matching ground truth within tolerance.
 *   A4 — every wire-contract §7 instrument for all six endpoints is present
 *        in a telemetry capture (test-local xmBase capture binding — see
 *        capture_binding.hpp for why not the full SDK sink), with correct
 *        instrument kinds, values reconciling with introspect::, and the
 *        AMENDED deadline_miss semantics (one event per Fresh->Stale
 *        TRANSITION, not per stale take). §7 label/unit metadata is not
 *        expressible through the xmBase metric API — stated divergence,
 *        recorded in detail/in_process.hpp.
 *   A5 — messaging stamps interleave coherently on the one family clock: a
 *        derived publish never precedes the publish of its input; every
 *        take follows its value's stamp.
 *
 * Test conditions stated per family rule: in-process reach; deterministic
 * single-threaded pipeline steps (the criteria are about lineage arithmetic
 * and accounting, not scheduling; racing coverage is M1's job); POD
 * payloads shaped like the M13 vocabulary.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "capture_binding.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using namespace std::chrono_literals;

namespace {

struct PoseSample {
  double x, y, yaw;
  std::uint64_t seq;
};
struct StateEstimate {
  double x, y, yaw, vx, vy;
  std::uint64_t seq;
};
struct CmdSample {
  double v, w;
  std::uint64_t seq;
};

// The six-endpoint pipeline. Domain first so endpoints die before it.
struct Pipeline {
  msg::Domain domain;
  msg::Publisher<PoseSample> pose_pub;    // stage 1: sensor (origin)
  msg::Subscriber<PoseSample> pose_sub;   // stage 2 input
  msg::Publisher<StateEstimate> state_pub;  // stage 2: estimator
  msg::Subscriber<StateEstimate> state_sub;  // stage 3 input
  msg::Publisher<CmdSample> cmd_pub;      // stage 3: controller
  msg::Subscriber<CmdSample> cmd_sub;     // recorder
};

Pipeline Wire(const char* domain_name, const std::string& prefix) {
  auto domain = msg::Domain::InProcess({.name = domain_name});
  auto pose_pub = domain.Advertise<PoseSample>(
      prefix + ".sensor.pose", {.history = msg::History::LatestOnly()});
  auto pose_sub = domain.Subscribe<PoseSample>(
      prefix + ".sensor.pose", {.history = msg::History::LatestOnly()});
  auto state_pub = domain.Advertise<StateEstimate>(
      prefix + ".estimator.state", {.history = msg::History::LatestOnly()});
  auto state_sub = domain.Subscribe<StateEstimate>(
      prefix + ".estimator.state", {.history = msg::History::LatestOnly()});
  auto cmd_pub = domain.Advertise<CmdSample>(
      prefix + ".controller.cmd", {.history = msg::History::LatestOnly()});
  auto cmd_sub = domain.Subscribe<CmdSample>(
      prefix + ".controller.cmd", {.history = msg::History::LatestOnly()});
  return Pipeline{std::move(domain),   std::move(pose_pub),
                  std::move(pose_sub), std::move(state_pub),
                  std::move(state_sub), std::move(cmd_pub),
                  std::move(cmd_sub)};
}

void PublishPose(Pipeline& p, std::uint64_t seq) {
  ASSERT_EQ(p.pose_pub.Publish(PoseSample{1.0, 2.0, 0.5, seq}),
            msg::PublishStatus::kOk);
}

// Stage 2: consume the given pose sample, publish the derived estimate
// (D14: one verb carries the lineage — no bookkeeping in this "component").
void EstimateFrom(Pipeline& p, const msg::Sample<PoseSample>& pose) {
  auto loan = p.state_pub.Loan();
  ASSERT_EQ(loan.status(), msg::LoanStatus::kOk);
  loan->x = (*pose).x;
  loan->y = (*pose).y;
  loan->yaw = (*pose).yaw;
  loan->vx = 0.1;
  loan->vy = 0.0;
  loan->seq = (*pose).seq;
  ASSERT_EQ(p.state_pub.PublishDerived(std::move(loan), pose),
            msg::PublishStatus::kOk);
}

// Stage 3: consume the estimate, publish the derived command.
void CommandFrom(Pipeline& p, const msg::Sample<StateEstimate>& est) {
  auto loan = p.cmd_pub.Loan();
  ASSERT_EQ(loan.status(), msg::LoanStatus::kOk);
  loan->v = (*est).vx;
  loan->w = (*est).yaw;
  loan->seq = (*est).seq;
  ASSERT_EQ(p.cmd_pub.PublishDerived(std::move(loan), est),
            msg::PublishStatus::kOk);
}

}  // namespace

// -- A1: origin_age and age provably diverge under the estimator stall -------
TEST(M13Behavioral, A1_OriginAgeDivergesFromAgeUnderStall) {
  auto p = Wire("m13_a1", "m13");

  // Healthy cycle: value age and information age agree (within code time).
  PublishPose(p, 1);
  auto pose = p.pose_sub.TakeLatest();
  ASSERT_EQ(pose.freshness(), msg::Freshness::kFresh);
  EstimateFrom(p, pose);
  auto est = p.state_sub.TakeLatest();
  ASSERT_EQ(est.freshness(), msg::Freshness::kFresh);
  EXPECT_LT(est.origin_age() - est.age(), 50ms);

  // Injected stall (M13-A1): the sensor input ages 300 ms while the
  // estimator keeps republishing its last fusion of the OLD pose.
  constexpr auto kStall = 300ms;
  std::this_thread::sleep_for(kStall);
  EstimateFrom(p, pose);  // re-derive from the stale input
  auto stalled = p.state_sub.TakeLatest();
  ASSERT_EQ(stalled.freshness(), msg::Freshness::kFresh);

  // The scenario's reason to exist: a fresh VALUE (small age — it was just
  // published) carrying stale INFORMATION (origin_age grew by the stall).
  EXPECT_LT(stalled.age(), 100ms);
  EXPECT_GE(stalled.origin_age(), kStall);
  EXPECT_GE(stalled.origin_age() - stalled.age(), kStall - 50ms);
  // The gate a controller would write, verbatim from the wish-code:
  constexpr auto kMaxInformationAge = 150ms;
  EXPECT_GT(stalled.origin_age(), kMaxInformationAge);   // would degrade
  EXPECT_LT(stalled.age(), kMaxInformationAge);          // age alone lies
}

// -- A2: hop count increments per stage; first hop has origin == stamp -------
TEST(M13Behavioral, A2_HopCountAndFirstHopOrigin) {
  auto p = Wire("m13_a2", "m13");

  PublishPose(p, 7);
  auto pose = p.pose_sub.TakeLatest();
  ASSERT_EQ(pose.freshness(), msg::Freshness::kFresh);
  // First-hop publish: lineage starts free (M13-A2).
  EXPECT_EQ(pose.hop_count(), 0u);
  EXPECT_EQ(pose.origin_stamp(), pose.stamp());

  EstimateFrom(p, pose);
  auto est = p.state_sub.TakeLatest();
  ASSERT_EQ(est.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ(est.hop_count(), 1u);                     // sensor(0) -> +1
  EXPECT_EQ(est.origin_stamp(), pose.stamp());        // origin preserved
  EXPECT_GE(est.stamp(), pose.stamp());               // new publish stamp

  CommandFrom(p, est);
  auto cmd = p.cmd_sub.TakeLatest();
  ASSERT_EQ(cmd.freshness(), msg::Freshness::kFresh);
  EXPECT_EQ(cmd.hop_count(), 2u);                     // estimator -> +1
  EXPECT_EQ(cmd.origin_stamp(), pose.stamp());        // still the sensor's
  EXPECT_EQ((*cmd).seq, 7u);                          // the same information
}

// -- A3: end-to-end latency computable from envelope stamps at the consumer --
TEST(M13Behavioral, A3_EndToEndLatencyFromRecordedStamps) {
  auto p = Wire("m13_a3", "m13");

  constexpr int kCycles = 50;
  std::vector<xmotion::Duration> from_envelope;  // recorder-side records only
  std::vector<xmotion::Duration> ground_truth;   // test-side wall measurement
  from_envelope.reserve(kCycles);
  ground_truth.reserve(kCycles);

  for (int i = 0; i < kCycles; ++i) {
    const auto t_before_publish = xmotion::Now();
    PublishPose(p, static_cast<std::uint64_t>(i));
    auto pose = p.pose_sub.TakeLatest();
    ASSERT_EQ(pose.freshness(), msg::Freshness::kFresh);
    EstimateFrom(p, pose);
    auto est = p.state_sub.TakeLatest();
    ASSERT_EQ(est.freshness(), msg::Freshness::kFresh);
    CommandFrom(p, est);
    auto cmd = p.cmd_sub.TakeLatest();
    ASSERT_EQ(cmd.freshness(), msg::Freshness::kFresh);
    // What the recorder can compute from the envelope alone: now - origin.
    // origin_age() IS that computation, published surface of the stamps.
    from_envelope.push_back(cmd.origin_age());
    ground_truth.push_back(xmotion::Now() - t_before_publish);
  }

  for (int i = 0; i < kCycles; ++i) {
    // The envelope-derived latency is bounded by the ground-truth window
    // (origin stamp is taken inside it) and agrees within tolerance —
    // shared clock, no calibration, no scenario-side instrumentation.
    EXPECT_LE(from_envelope[i], ground_truth[i]) << "cycle " << i;
    EXPECT_LT(ground_truth[i] - from_envelope[i], 5ms) << "cycle " << i;
  }
}

// -- A4: the full §7 instrument set, for all six endpoints, from capture -----
TEST(M13Behavioral, A4_StandardInstrumentSchemaPresentForAllEndpoints) {
  // Install the capture binding BEFORE wiring: instrument handles bind to
  // their slots at endpoint creation (xmBase contract).
  auto& capture = xmmsg_test::MetricCapture::Instance();
  capture.Install();

  auto p = Wire("m13_a4", "m13.a4");  // unique topics => exact slot values

  // Drive one full pipeline pass so counters/histograms have real traffic.
  PublishPose(p, 1);
  auto pose = p.pose_sub.TakeLatest();
  ASSERT_EQ(pose.freshness(), msg::Freshness::kFresh);
  EstimateFrom(p, pose);
  auto est = p.state_sub.TakeLatest();
  ASSERT_EQ(est.freshness(), msg::Freshness::kFresh);
  CommandFrom(p, est);
  auto cmd = p.cmd_sub.TakeLatest();
  ASSERT_EQ(cmd.freshness(), msg::Freshness::kFresh);

  using Kind = xmmsg_test::InstrumentKind;
  const std::string topics[3] = {"m13.a4.sensor.pose", "m13.a4.estimator.state",
                                 "m13.a4.controller.cmd"};
  for (const auto& topic : topics) {
    // Per publisher (§7): three counters.
    for (const char* inst : {"publish_count", "refused_count", "bytes"}) {
      EXPECT_TRUE(capture.Has("messaging.pub." + topic + "." + inst,
                              Kind::kCounter))
          << topic << " " << inst;
    }
    // Per subscriber (§7): four counters, one histogram, one gauge.
    for (const char* inst : {"take_count", "drop_count", "overwrite_count",
                             "deadline_miss_count"}) {
      EXPECT_TRUE(capture.Has("messaging.sub." + topic + "." + inst,
                              Kind::kCounter))
          << topic << " " << inst;
    }
    EXPECT_TRUE(capture.Has("messaging.sub." + topic + ".take_age_us",
                            Kind::kHistogram))
        << topic;
    EXPECT_TRUE(
        capture.Has("messaging.sub." + topic + ".queue_depth", Kind::kGauge))
        << topic;
    // Per hop (§7): emitted because in-process shares one clock (R8).
    EXPECT_TRUE(capture.Has("messaging.hop." + topic + ".hop_latency_us",
                            Kind::kHistogram))
        << topic;
  }
  // Per domain (§7): two gauges, maintained at wiring time.
  EXPECT_TRUE(capture.Has("messaging.domain.endpoint_count", Kind::kGauge));
  EXPECT_TRUE(capture.Has("messaging.domain.match_count", Kind::kGauge));
  EXPECT_DOUBLE_EQ(capture.GaugeValue("messaging.domain.endpoint_count"), 6.0);
  EXPECT_DOUBLE_EQ(capture.GaugeValue("messaging.domain.match_count"), 3.0);

  // The captured values reconcile with the D9 introspection counters (the
  // library emitted them; this file plumbed nothing).
  EXPECT_DOUBLE_EQ(
      capture.CounterValue("messaging.pub.m13.a4.sensor.pose.publish_count"),
      static_cast<double>(msg::introspect::PublishCount(p.pose_pub)));
  EXPECT_DOUBLE_EQ(
      capture.CounterValue("messaging.sub.m13.a4.controller.cmd.take_count"),
      static_cast<double>(msg::introspect::TakeCount(p.cmd_sub)));
  EXPECT_EQ(
      capture.HistogramCount("messaging.hop.m13.a4.controller.cmd.hop_latency_us"),
      msg::introspect::TakeCount(p.cmd_sub));

  // §7 amended deadline_miss semantics: one event per Fresh->Stale
  // TRANSITION — repeated stale takes do not inflate it; recovery re-arms.
  const std::string miss_name =
      "messaging.sub.m13.a4.deadline.probe.deadline_miss_count";
  auto miss_pub = p.domain.Advertise<PoseSample>(
      "m13.a4.deadline.probe", {.history = msg::History::LatestOnly()});
  auto miss_sub = p.domain.Subscribe<PoseSample>(
      "m13.a4.deadline.probe",
      {.history = msg::History::LatestOnly(), .deadline = 50ms});
  ASSERT_EQ(miss_pub.Publish(PoseSample{0, 0, 0, 1}), msg::PublishStatus::kOk);
  ASSERT_EQ(miss_sub.TakeLatest().freshness(), msg::Freshness::kFresh);
  std::this_thread::sleep_for(120ms);
  ASSERT_EQ(miss_sub.TakeLatest().freshness(), msg::Freshness::kStale);
  ASSERT_EQ(miss_sub.TakeLatest().freshness(), msg::Freshness::kStale);
  EXPECT_DOUBLE_EQ(capture.CounterValue(miss_name), 1.0)
      << "deadline_miss must count transitions, not stale takes";
  ASSERT_EQ(miss_pub.Publish(PoseSample{0, 0, 0, 2}), msg::PublishStatus::kOk);
  ASSERT_EQ(miss_sub.TakeLatest().freshness(), msg::Freshness::kFresh);
  std::this_thread::sleep_for(120ms);
  ASSERT_EQ(miss_sub.TakeLatest().freshness(), msg::Freshness::kStale);
  EXPECT_DOUBLE_EQ(capture.CounterValue(miss_name), 2.0)
      << "recovery must re-arm the transition event";
}

// -- A5: one timeline — stamps and their causal order never contradict -------
TEST(M13Behavioral, A5_StampTimelineCoherence) {
  auto p = Wire("m13_a5", "m13");

  const auto t_start = xmotion::Now();
  PublishPose(p, 1);
  auto pose = p.pose_sub.TakeLatest();
  ASSERT_EQ(pose.freshness(), msg::Freshness::kFresh);
  const auto t_pose_taken = xmotion::Now();
  EstimateFrom(p, pose);
  auto est = p.state_sub.TakeLatest();
  ASSERT_EQ(est.freshness(), msg::Freshness::kFresh);
  const auto t_est_taken = xmotion::Now();
  CommandFrom(p, est);
  auto cmd = p.cmd_sub.TakeLatest();
  ASSERT_EQ(cmd.freshness(), msg::Freshness::kFresh);
  const auto t_cmd_taken = xmotion::Now();

  // Everything is on the one family clock (R11): a derived publish never
  // precedes the publish of its input, no take precedes its value's stamp,
  // and the origin stamp brackets the whole chain from below.
  EXPECT_GE(pose.stamp(), t_start);
  EXPECT_GE(t_pose_taken, pose.stamp());
  EXPECT_GE(est.stamp(), pose.stamp());   // hop after its cause
  EXPECT_GE(t_est_taken, est.stamp());
  EXPECT_GE(cmd.stamp(), est.stamp());    // hop after its cause
  EXPECT_GE(t_cmd_taken, cmd.stamp());
  EXPECT_EQ(cmd.origin_stamp(), pose.stamp());
  EXPECT_LE(cmd.origin_stamp(), cmd.stamp());
}

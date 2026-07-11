/*
 * m7_behavioral_test.cpp — M7: trace continuity across the hop (P0b,
 * in-process leg).
 *
 * Asserts the M7 acceptance criteria from docs/scenarios.md against the
 * real in-process reach:
 *   A1 — one TraceId across the hop: the producer's context (NewTrace +
 *        ContextGuard, zero plumbing at Publish) rides in the envelope;
 *        asserted from the ENVELOPE BYTES (via the detail test seam) and
 *        from the extracted Sample::context(); consumer adoption is one
 *        explicit RAII scope (D13/D20: xmBase ContextGuard).
 *   A2 — two interleaved traces never cross-contaminate: the overwritten
 *        value's context vanishes with it (the context travels with the
 *        value, never ambiently), and adoption never reparents the
 *        consumer's own trace.
 *   A3 — envelope overhead is fixed-size, with or without an active trace
 *        (a no-trace publish carries the null context — valid bytes, never
 *        garbage).
 *
 * Test conditions stated per family rule: in-process reach; producer thread
 * owned by the test (A1) with deterministic single-threaded bodies
 * elsewhere; TrajectoryHead-shaped POD payload.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <thread>

#include "xmbase/telemetry/context.hpp"
#include "xmmessaging/messaging.hpp"

namespace msg = xmotion::messaging;
namespace tel = xmotion::telemetry;
using namespace std::chrono_literals;

namespace {

struct TrajectoryHead {
  std::uint64_t plan_id;
  double x[8];
};

// Read the current master-slot envelope of a latest-only topic through the
// detail test seam (EndpointAccess) — M7-A1 is a claim about the BYTES on
// the wire, not only about the extracted surface.
msg::detail::Envelope MasterEnvelopeOf(
    const msg::Subscriber<TrajectoryHead>& sub) {
  auto* impl = static_cast<msg::detail::SubImpl<TrajectoryHead>*>(
      msg::detail::EndpointAccess::Get(sub));
  auto* topic =
      static_cast<msg::detail::Topic<TrajectoryHead>*>(impl->topic_.get());
  msg::detail::MailRecord<TrajectoryHead> record;
  EXPECT_TRUE(topic->LoadMaster(record));
  return record.envelope;
}

}  // namespace

// -- A1: one TraceId across the hop, from the envelope bytes -----------------
TEST(M7Behavioral, A1_OneTraceIdAcrossTheHop) {
  auto domain = msg::Domain::InProcess({.name = "m7_a1"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});
  ASSERT_EQ(pub.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(sub.status(), msg::SubscribeStatus::kOk);

  // Producer: an ordinary instrumented iteration on its own thread. Note
  // the publish site mentions no context — capture is automatic (D13).
  tel::Context producer_ctx{};
  std::thread planner([&] {
    producer_ctx = tel::NewTrace();
    tel::ContextGuard iteration(producer_ctx);
    pub.Publish(TrajectoryHead{42, {}});
  });
  planner.join();

  // The envelope bytes ARE the producer's Inject bytes (M7-A1).
  const msg::detail::Envelope envelope = MasterEnvelopeOf(sub);
  const auto expected_bytes = tel::Inject(producer_ctx);
  EXPECT_EQ(0, std::memcmp(envelope.context, expected_bytes.data(),
                           msg::detail::kEnvelopeContextSize));

  // The extracted surface agrees with the bytes.
  auto plan = sub.TakeLatest();
  ASSERT_EQ(plan.freshness(), msg::Freshness::kFresh);
  ASSERT_TRUE(plan.context().valid());
  EXPECT_EQ(plan.context().trace, producer_ctx.trace);
  EXPECT_EQ(plan.context().span, producer_ctx.span);

  // Adoption is one explicit RAII scope (D13/D20): inside it the consumer
  // thread carries the producer's trace; at scope exit the prior context
  // is restored exactly.
  const tel::Context before = tel::CurrentContext();
  {
    tel::ContextGuard hop(plan.context());
    EXPECT_EQ(tel::CurrentContext().trace, producer_ctx.trace);
  }
  EXPECT_EQ(tel::CurrentContext().trace, before.trace);
}

// -- A2: interleaved traces never cross-contaminate ---------------------------
TEST(M7Behavioral, A2_InterleavedTracesNeverCross) {
  auto domain = msg::Domain::InProcess({.name = "m7_a2"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});

  // Two iterations, two traces, one latest-only topic: the second publish
  // overwrites the first — and the first's context must vanish WITH the
  // overwritten value (the envelope travels with the value, not ambiently).
  const tel::Context trace_a = tel::NewTrace();
  {
    tel::ContextGuard guard(trace_a);
    pub.Publish(TrajectoryHead{1, {}});
  }
  const tel::Context trace_b = tel::NewTrace();
  {
    tel::ContextGuard guard(trace_b);
    pub.Publish(TrajectoryHead{2, {}});
  }

  // The consumer runs its OWN trace; adopting the hop context must not
  // reparent it (M7-A2 — the telemetry S2-A5 discipline across transport).
  const tel::Context consumer_own = tel::NewTrace();
  {
    tel::ContextGuard own(consumer_own);
    auto plan = sub.TakeLatest();
    ASSERT_EQ(plan.freshness(), msg::Freshness::kFresh);
    EXPECT_EQ((*plan).plan_id, 2u);                    // value B...
    EXPECT_EQ(plan.context().trace, trace_b.trace);    // ...with B's trace
    EXPECT_FALSE(plan.context().trace == trace_a.trace)
        << "overwritten value's context leaked onto the surviving value";
    {
      tel::ContextGuard hop(plan.context());
      EXPECT_EQ(tel::CurrentContext().trace, trace_b.trace);
    }
    // Scope closed: the consumer's own trace is intact, uncontaminated.
    EXPECT_EQ(tel::CurrentContext().trace, consumer_own.trace);
  }
}

// -- A3: fixed-size envelope, with and without an active trace ---------------
TEST(M7Behavioral, A3_EnvelopeSizeInvariant) {
  // The envelope is a compile-time fixed-size contract; these mirror the
  // static_asserts in detail/envelope.hpp as an executable record.
  EXPECT_EQ(sizeof(msg::detail::Envelope), 48u);
  EXPECT_EQ(msg::detail::kEnvelopeContextSize, 24u);
  // Per-record transport overhead is envelope + ordinal, payload aside —
  // identical for every value on the topic, trace or no trace.
  EXPECT_EQ(sizeof(msg::detail::MailRecord<TrajectoryHead>),
            sizeof(msg::detail::Envelope) + sizeof(std::uint64_t) +
                sizeof(TrajectoryHead));

  auto domain = msg::Domain::InProcess({.name = "m7_a3"});
  auto pub = domain.Advertise<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<TrajectoryHead>(
      "m7.plan.head", {.history = msg::History::LatestOnly()});

  // No active trace: the context field is the NULL context — valid bytes,
  // never garbage (and Extract of them is the invalid() Context).
  ASSERT_FALSE(tel::CurrentContext().valid());  // test runs untraced
  pub.Publish(TrajectoryHead{1, {}});
  const msg::detail::Envelope untraced = MasterEnvelopeOf(sub);
  const std::uint8_t zeros[msg::detail::kEnvelopeContextSize] = {};
  EXPECT_EQ(0, std::memcmp(untraced.context, zeros, 16))
      << "no-trace publish must carry the all-zero trace id";
  auto plain = sub.TakeLatest();
  ASSERT_EQ(plain.freshness(), msg::Freshness::kFresh);
  EXPECT_FALSE(plain.context().valid());

  // With a trace: same envelope, same size, context bytes populated.
  const tel::Context ctx = tel::NewTrace();
  {
    tel::ContextGuard guard(ctx);
    pub.Publish(TrajectoryHead{2, {}});
  }
  auto traced = sub.TakeLatest();
  ASSERT_EQ(traced.freshness(), msg::Freshness::kFresh);
  EXPECT_TRUE(traced.context().valid());
}

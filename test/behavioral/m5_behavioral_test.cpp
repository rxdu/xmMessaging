/*
 * m5_behavioral_test.cpp — M5: request/response with deadline (P0b,
 * in-process).
 *
 * Asserts the M5 acceptance criteria from docs/scenarios.md against the
 * real in-process reach:
 *   A1 — prompt case: typed response; the request's telemetry context
 *        (D13, captured automatically at Call) reaches the server handler
 *        through the envelope, extracted via Request::context().
 *   A2 — slow case: kDeadlineExpired at the deadline (±10%); the late
 *        reply is discarded by correlation (server observes
 *        ReplyStatus::kExpired, D20) and NEVER surfaces on a later call.
 *   A3 — absent case: kNoServer, fast-fail, distinct from timeout.
 *   A4 — a deadline-less Call is unrepresentable — compile-time, asserted
 *        by a static (detection-idiom) probe below.
 *   Plus the D10 bounded-park verb: WaitForWorkOrShutdown returns within
 *        max_park with no work (false) and promptly when work arrives
 *        (true) — the server loop neither busy-spins nor sleeps blind.
 *
 * Test conditions stated per family rule: in-process reach; the tests own
 * every thread (R3 — the server executes only inside TakeRequest/Reply on
 * a test-owned loop); payloads are the M5 GainsRequest/GainsResponse PODs.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include "xmbase/telemetry/context.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
namespace tel = xmotion::telemetry;
using namespace std::chrono_literals;

namespace {

struct GainsRequest {
  std::uint32_t mode;
};
struct GainsResponse {
  double kp, ki, kd;
};

GainsResponse GainsFor(std::uint32_t mode) {
  return GainsResponse{static_cast<double>(mode) * 10.0, 0.1, 0.01};
}

// -- M5-A4: the compile-time contract, asserted statically -------------------
// D11: Call(req, deadline) has no deadline-less overload and no default
// argument — an unbounded wait is unrepresentable. This cannot be a runtime
// test; the detection idiom below is the static probe: if anyone ever adds
// `client.Call(req)` viability (overload or default argument), this
// static_assert fails the build.
template <typename C, typename R, typename = void>
struct CallableWithoutDeadline : std::false_type {};
template <typename C, typename R>
struct CallableWithoutDeadline<
    C, R,
    std::void_t<decltype(std::declval<C&>().Call(std::declval<const R&>()))>>
    : std::true_type {};

static_assert(
    !CallableWithoutDeadline<msg::Client<GainsRequest, GainsResponse>,
                             GainsRequest>::value,
    "M5-A4 violated: Client::Call became callable without a deadline");

}  // namespace

// -- A1: prompt typed response, context propagated into the handler ----------
TEST(M5Behavioral, A1_PromptResponseWithContextPropagation) {
  auto domain = msg::Domain::InProcess({.name = "m5_a1"});
  auto server = domain.Serve<GainsRequest, GainsResponse>("m5.config.get_gains");
  auto client = domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");
  ASSERT_EQ(server.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(client.status(), msg::SubscribeStatus::kOk);

  // The caller's trace, minted like any instrumented iteration (D13: Call
  // snapshots it into the request envelope with zero plumbing).
  const tel::Context caller_ctx = tel::NewTrace();

  std::atomic<bool> done{false};
  tel::Context handler_ctx{};      // written before `served`, read after
  msg::ReplyStatus reply_status{}; // idem
  std::atomic<bool> served{false};

  std::thread server_thread([&] {
    while (!done.load()) {
      auto req = server.TakeRequest();
      if (req.freshness() != msg::Freshness::kNone) {
        // M5-A1: the caller's context, extracted from the envelope bytes.
        handler_ctx = req.context();
        reply_status = server.Reply(req, GainsFor((*req).mode));
        served.store(true, std::memory_order_release);
      }
      server.WaitForWorkOrShutdown(5ms);  // D10: bounded park, no busy spin
    }
  });

  msg::Result<GainsResponse> result;
  {
    tel::ContextGuard guard(caller_ctx);
    result = client.Call(GainsRequest{2}, 500ms);
  }
  done.store(true);
  server_thread.join();

  ASSERT_EQ(result.status(), msg::CallStatus::kOk);
  EXPECT_DOUBLE_EQ((*result).kp, 20.0);  // typed response, correct payload
  EXPECT_DOUBLE_EQ((*result).ki, 0.1);
  EXPECT_DOUBLE_EQ((*result).kd, 0.01);

  ASSERT_TRUE(served.load(std::memory_order_acquire));
  EXPECT_EQ(reply_status, msg::ReplyStatus::kOk);
  // One TraceId across the hop (the handler saw the caller's context).
  EXPECT_TRUE(handler_ctx.valid());
  EXPECT_EQ(handler_ctx.trace, caller_ctx.trace);
}

// -- A2: timeout at the deadline; late reply discarded, never resurfacing ----
TEST(M5Behavioral, A2_SlowServerTimeoutAndLateReplyDiscarded) {
  constexpr auto kDeadline = 200ms;
  auto domain = msg::Domain::InProcess({.name = "m5_a2"});
  auto server = domain.Serve<GainsRequest, GainsResponse>("m5.config.get_gains");
  auto client = domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");
  ASSERT_EQ(server.status(), msg::AdvertiseStatus::kOk);
  ASSERT_EQ(client.status(), msg::SubscribeStatus::kOk);

  std::atomic<bool> done{false};
  std::atomic<bool> late_reply_sent{false};
  msg::ReplyStatus late_reply_status{};  // written before the flag above

  std::thread server_thread([&] {
    // First request: deliberately slower than the caller's deadline.
    for (;;) {
      auto req = server.TakeRequest();
      if (req.freshness() != msg::Freshness::kNone) {
        std::this_thread::sleep_for(kDeadline * 3);  // way past the deadline
        late_reply_status = server.Reply(req, GainsFor((*req).mode));
        late_reply_sent.store(true, std::memory_order_release);
        break;
      }
      server.WaitForWorkOrShutdown(5ms);
    }
    // Then serve promptly until told to stop.
    while (!done.load()) {
      auto req = server.TakeRequest();
      if (req.freshness() != msg::Freshness::kNone) {
        server.Reply(req, GainsFor((*req).mode));
      }
      server.WaitForWorkOrShutdown(5ms);
    }
  });

  // The slow call: expires at the deadline, ±10% (M5-A2).
  const auto t0 = xmotion::Now();
  auto r1 = client.Call(GainsRequest{1}, kDeadline);
  const auto elapsed = xmotion::Now() - t0;
  EXPECT_EQ(r1.status(), msg::CallStatus::kDeadlineExpired);
  EXPECT_GE(elapsed, kDeadline);  // Call may never give up early
  EXPECT_LE(elapsed, kDeadline + kDeadline / 10);

  // Wait until the late reply has actually been sent and discarded, so the
  // next call genuinely races nothing.
  while (!late_reply_sent.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(1ms);
  }
  // D20: the server-side discard is observable, not an error path.
  EXPECT_EQ(late_reply_status, msg::ReplyStatus::kExpired);

  // M5-A2, the load-bearing half: the late reply for mode=1 must NEVER
  // surface on this later call for mode=7.
  auto r2 = client.Call(GainsRequest{7}, 500ms);
  done.store(true);
  server_thread.join();

  ASSERT_EQ(r2.status(), msg::CallStatus::kOk);
  EXPECT_DOUBLE_EQ((*r2).kp, 70.0) << "a late reply surfaced on a later call";
}

// -- A3: absent server fails fast with a distinct status ---------------------
TEST(M5Behavioral, A3_AbsentServerFailsFast) {
  auto domain = msg::Domain::InProcess({.name = "m5_a3"});
  auto client = domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");
  ASSERT_EQ(client.status(), msg::SubscribeStatus::kOk);

  const auto t0 = xmotion::Now();
  auto result = client.Call(GainsRequest{1}, 500ms);
  const auto elapsed = xmotion::Now() - t0;

  // Distinct from timeout ("config server crashed" != "overloaded"), and
  // fast: nothing waits for a deadline that cannot be met.
  EXPECT_EQ(result.status(), msg::CallStatus::kNoServer);
  EXPECT_LT(elapsed, 50ms);
}

// -- In-flight bound: callers beyond the preallocated slots park, not fail ---
TEST(M5Behavioral, InFlightBound_ConcurrentCallersBeyondSlots) {
  // The RPC transport preallocates kMaxInFlight = 8 call slots per topic
  // (detail/in_process.hpp — the documented default; a Qos knob is a P1
  // decision). More concurrent callers than slots is legal: the surplus
  // callers park until a slot frees, bounded by their own deadlines. 12
  // caller threads x 20 calls each also exercises slot reuse and
  // correlation-token turnover under real contention (TSan-relevant).
  auto domain = msg::Domain::InProcess({.name = "m5_bound"});
  auto server = domain.Serve<GainsRequest, GainsResponse>("m5.config.get_gains");
  ASSERT_EQ(server.status(), msg::AdvertiseStatus::kOk);

  std::atomic<bool> done{false};
  std::thread server_thread([&] {
    while (!done.load()) {
      for (;;) {  // drain every pending request before parking again
        auto req = server.TakeRequest();
        if (req.freshness() == msg::Freshness::kNone) {
          break;
        }
        server.Reply(req, GainsFor((*req).mode));
      }
      server.WaitForWorkOrShutdown(5ms);
    }
  });

  constexpr int kCallers = 12;  // > kMaxInFlight
  constexpr int kCallsEach = 20;
  std::atomic<std::uint32_t> ok_calls{0};
  std::atomic<std::uint32_t> wrong_responses{0};
  std::vector<std::thread> callers;
  callers.reserve(kCallers);
  for (int t = 0; t < kCallers; ++t) {
    callers.emplace_back([&domain, &ok_calls, &wrong_responses, t] {
      auto client =
          domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");
      for (int i = 0; i < kCallsEach; ++i) {
        const std::uint32_t mode =
            static_cast<std::uint32_t>(t * 100 + i + 1);
        auto result = client.Call(GainsRequest{mode}, 5000ms);
        if (result.status() == msg::CallStatus::kOk) {
          ok_calls.fetch_add(1);
          if ((*result).kp != static_cast<double>(mode) * 10.0) {
            wrong_responses.fetch_add(1);  // a crossed correlation
          }
        }
      }
    });
  }
  for (auto& caller : callers) {
    caller.join();
  }
  done.store(true);
  server_thread.join();

  // With a prompt server and generous deadlines, every call completes and
  // every response matches ITS request — no crossed correlations, ever.
  EXPECT_EQ(ok_calls.load(), kCallers * kCallsEach);
  EXPECT_EQ(wrong_responses.load(), 0u);
}

// -- D10: the bounded-park verb ----------------------------------------------
TEST(M5Behavioral, D10_WaitForWorkOrShutdownIsBounded) {
  auto domain = msg::Domain::InProcess({.name = "m5_d10"});
  auto server = domain.Serve<GainsRequest, GainsResponse>("m5.config.get_gains");
  ASSERT_EQ(server.status(), msg::AdvertiseStatus::kOk);

  // No work: returns false, having parked ~max_park (bounded, not forever;
  // not a busy spin — the park is a single futex wait, not a poll loop).
  const auto t0 = xmotion::Now();
  EXPECT_FALSE(server.WaitForWorkOrShutdown(50ms));
  const auto parked = xmotion::Now() - t0;
  EXPECT_GE(parked, 45ms);
  EXPECT_LT(parked, 500ms);

  // Work arrives mid-park: returns true well before max_park.
  auto client = domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");
  std::thread caller([&] {
    auto result = client.Call(GainsRequest{3}, 2000ms);
    EXPECT_EQ(result.status(), msg::CallStatus::kOk);
    EXPECT_DOUBLE_EQ((*result).kp, 30.0);
  });

  const auto t1 = xmotion::Now();
  bool work = server.WaitForWorkOrShutdown(2000ms);
  const auto waited = xmotion::Now() - t1;
  EXPECT_TRUE(work);
  EXPECT_LT(waited, 1000ms);  // woke on arrival, not on max_park

  auto req = server.TakeRequest();
  ASSERT_NE(req.freshness(), msg::Freshness::kNone);
  EXPECT_EQ((*req).mode, 3u);
  EXPECT_GE(req.age(), xmotion::Duration::zero());  // caller-context surface
  EXPECT_EQ(server.Reply(req, GainsFor((*req).mode)), msg::ReplyStatus::kOk);
  caller.join();
}

// M5 — Request/response with deadline (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M5.
//
// Contract under test: typed RPC that refuses to wait forever. Two design
// commitments visible here:
//   1. Deadlines are unrepresentable-to-omit: Call() has no overload
//      without one (M5-A4).
//   2. No hidden threads (R3): the server does not own execution. It is
//      take/reply — the application polls it from a thread IT owns, same
//      as every other loop in the family.

#include "xmmsg/messaging.hpp"

#include <thread>

namespace msg = xmotion::messaging;

struct GainsRequest  { uint32_t mode; };
struct GainsResponse { double kp, ki, kd; };

int main() {
  msg::Domain domain = msg::Domain::InProcess();

  // -- Server: the fake parameter server, app-owned loop --------------------
  auto server = domain.Serve<GainsRequest, GainsResponse>("m5.config.get_gains");

  std::thread server_thread([&] {
    while (running) {
      // Same Sample/freshness surface as pub/sub (D1/D2): an empty inbox
      // is kNone, not a different type. The taken request carries its
      // reply token and the caller's telemetry context — replying under
      // that context is what links the server's span into the client's
      // trace (M5-A1).
      if (auto req = server.TakeRequest(); req.freshness() != msg::Freshness::kNone) {
        server.Reply(req, GainsResponse{.kp = LookupKp((*req).mode), .ki = 0.1, .kd = 0.01});
      }
      WaitForWorkOrShutdown(server);  // bounded park, no busy spin
    }
  });

  // -- Client: mode-switch time, needs gains now-or-never -------------------
  auto client = domain.Client<GainsRequest, GainsResponse>("m5.config.get_gains");

  // The wish: a Result<T> mirroring Sample<T> — status + value, where the
  // value is only accessible on kOk. The deadline parameter has no default
  // argument; `client.Call({...})` must not compile.
  msg::Result<GainsResponse> r =
      client.Call({.mode = kModeTurtle}, std::chrono::milliseconds(50));

  switch (r.status()) {
    case msg::CallStatus::kOk:
      ApplyGains(*r);
      break;
    case msg::CallStatus::kDeadlineExpired:
      // The server exists but was too slow. The late reply, when it
      // arrives, is discarded by correlation — it must never surface on
      // a LATER call (M5-A2). Mode switch aborts, current gains hold.
      AbortModeSwitch("gains query timed out");
      break;
    case msg::CallStatus::kNoServer:
      // Nobody serves this topic — distinct from timeout (M5-A3), because
      // "config server crashed" and "config server overloaded" are
      // different incidents with different recoveries.
      AbortModeSwitch("no config server");
      break;
  }

  // Deferred, recorded as a delta: an async Call variant (poll-a-Result
  // handle) for callers that cannot block even boundedly. Not wished here —
  // no current consumer shape demands it, and it doubles the surface.

  running = false;
  server_thread.join();
  return 0;
}

// M3 — Slow consumer, back-pressure, flood (wish-code, P0.0)
//
// NOT COMPILED — API specification. See docs/scenarios.md M3.
//
// Contract under test: the Aeron lesson. Capacity exhaustion is explicit
// at the publish site (reliable) or exactly counted (best-effort); a
// flooded topic never degrades its neighbors. Publish NEVER blocks
// internally — "reliable" means the transport refuses visibly, and what
// to do about it (retry, coalesce, shed) is caller policy.

#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;

struct Sample64 { uint8_t bytes[64]; uint64_t seq; };

int main() {
  msg::Domain domain = msg::Domain::InProcess();

  // Topic A: reliable bounded queue. Overflow -> the PUBLISHER hears it.
  auto pub_a = domain.Advertise<Sample64>(
      "m3.reliable.stream",
      {.history = msg::History::Queue(8), .reliability = msg::Reliability::kReliable});
  auto sub_a = domain.Subscribe<Sample64>("m3.reliable.stream", {.history = msg::History::Queue(8)});

  // Topic B: best-effort bounded queue. Overflow -> counted drops.
  auto pub_b = domain.Advertise<Sample64>(
      "m3.besteffort.stream",
      {.history = msg::History::Queue(8), .reliability = msg::Reliability::kBestEffort});
  auto sub_b = domain.Subscribe<Sample64>("m3.besteffort.stream", {.history = msg::History::Queue(8)});

  // Topic C: healthy latest-only control traffic sharing the Domain —
  // the flood-isolation witness (M3-A3).
  auto pub_c = domain.Advertise<Sample64>("m3.control.heartbeat", {.history = msg::History::LatestOnly()});

  // -- Flood with both consumers stalled ------------------------------------
  uint64_t refused = 0, published_a = 0;
  for (uint64_t i = 0; i < 1000; ++i) {
    // The wish: one status enum, total accounting. kOk means the transport
    // accepted it; kWouldBlock means the bounded queue is full RIGHT NOW
    // and nothing was enqueued. There is no internal retry, no silent
    // wait, no exception — the RT caller stays in control of its cycle.
    switch (pub_a.Publish({.seq = i})) {
      case msg::PublishStatus::kOk:         ++published_a; break;
      case msg::PublishStatus::kWouldBlock: ++refused;     break;
    }
    // Best-effort: Publish always returns kOk (the value was accepted;
    // whether every subscriber kept it is the subscribers' queue policy —
    // per-subscriber overflow shows up in THEIR drop counters, because
    // "delivered to whom" is a per-endpoint fact, not a publisher fact).
    pub_b.Publish({.seq = i});
    pub_c.Publish({.seq = i});  // healthy neighbor keeps publishing
  }

  // M3-A1: exact conservation on the reliable topic.
  Assert(published_a + refused == 1000);
  DrainAll(sub_a);
  Assert(Seen(sub_a) == published_a);  // everything accepted arrives, in order

  // M3-A2: exact conservation on best-effort — delivered + counted drops.
  DrainAll(sub_b);
  Assert(Seen(sub_b) + msg::introspect::DropCount(sub_b) == 1000);

  // M3-A4: the same numbers are visible as messaging.* telemetry with no
  // scenario-side plumbing: m3.reliable.stream refusal count == refused,
  // m3.besteffort.stream drop count == DropCount(sub_b).

  // M3-A3 (behavioral version): topic C hop latency distribution under
  // flood is statistically indistinguishable from an unflooded baseline
  // run — one misbehaving stream cannot tax the control path.
  return 0;
}

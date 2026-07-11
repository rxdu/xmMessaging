/*
 * bench/xmmsg_bench.cpp — the M9 in-process benchmark suite (P0b).
 *
 * Three layers (docs/scenarios.md M9, docs/design.md R4):
 *  - micro:     per-verb cost — Publish (latest-only), Loan+Publish,
 *               TakeLatest (hit/miss), queue Publish/TryTake, RPC Call
 *               round-trip — at payload sizes {64 B, 1 KiB, 64 KiB}.
 *               (1 MiB is skipped at P0b: LoanPool inlines 8 cells per
 *               publisher core, i.e. 8 MiB of wiring-time storage per
 *               1 MiB topic — revisit with QoS-declared pool sizing, P1.)
 *  - path:      producer/consumer hop latency tails (p50/p99/p99.9/max) at
 *               the M1 profile (10 Hz plan / 1 kHz control) and at
 *               saturation, plus pacing jitter.
 *  - system:    the R4 robot-typical profile — 30 topics at mixed
 *               10 Hz–1 kHz rates, concurrent, per-topic hop tails under
 *               the combined load (single-topic sweeps flatter transports).
 *  - contended: the M9-A6 variants — a writer racing 4 readers on one
 *               slot, producer/consumer racing on one queue, 4 clients on
 *               one server, and the M3 flood profile running concurrently
 *               with a measured 1 kHz control topic (R7: WCET under
 *               contention, not idle runs).
 *
 * M9-A3: every publish/take hot-path measured section runs under the
 * behavioral suite's AllocProbe; a single allocation fails the run. The
 * lock-free half of A3 is a compile-time fact asserted below: these
 * payloads take the seqlock LatestSlot (never MutexLatestSlot) and the
 * SPSC BoundedQueue — both lock-free by construction, TSan-verified by the
 * behavioral suite.
 *
 * R3 note: at P0b the in-process reach IS the reference — there is no raw
 * backend to A/B against, so this suite records ABSOLUTE numbers; the
 * wrapper-overhead A/B lands with the first backend (P1).
 *
 * Harness: hand-rolled (bench/harness.hpp) — the family vendors no
 * google-benchmark, and telemetry's perf tier set the precedent.
 *
 * Bench code is exempt from the no-allocation rule EXCEPT inside measured
 * sections: every buffer is preallocated before its probe starts.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "alloc_probe.hpp"  // ONE TU per binary: defines operator new/delete
#include "harness.hpp"
#include "xmmsg/messaging.hpp"

namespace msg = xmotion::messaging;
using xmmsg_bench::BenchResult;
using xmmsg_bench::ComputeStats;
using xmmsg_bench::Stats;
using xmotion::Duration;
using xmotion::Now;
using xmotion::Timestamp;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Payloads. POD, exact-size, seq embedded for hop detection.
// ---------------------------------------------------------------------------

template <std::size_t kBytes>
struct Payload {
  static_assert(kBytes >= 16, "payload floor");
  std::uint64_t seq;
  unsigned char pad[kBytes - sizeof(std::uint64_t)];
};
static_assert(sizeof(Payload<64>) == 64);
static_assert(sizeof(Payload<1024>) == 1024);
static_assert(sizeof(Payload<65536>) == 65536);

// M9-A3 lock-free proof, compile-time half: a trivially copyable mail record
// selects the seqlock LatestSlot (wait-free reads/writes, no mutex — see
// detail::LatestSlotFor); a non-trivially-copyable one would silently fall
// back to MutexLatestSlot and this suite would be benchmarking a lock.
static_assert(
    std::is_trivially_copyable_v<msg::detail::MailRecord<Payload<64>>> &&
        std::is_trivially_copyable_v<msg::detail::MailRecord<Payload<1024>>> &&
        std::is_trivially_copyable_v<msg::detail::MailRecord<Payload<65536>>>,
    "M9-A3: bench payloads must take the lock-free seqlock slot path");

const char* SizeLabel(std::size_t bytes) {
  switch (bytes) {
    case 64:
      return "64B";
    case 1024:
      return "1KiB";
    case 65536:
      return "64KiB";
    default:
      return "?";
  }
}

// Optimization sink: measured take results funnel here so the compiler
// cannot elide the copy-out.
volatile std::uint64_t g_sink = 0;

// ---------------------------------------------------------------------------
// Run configuration + result registry.
// ---------------------------------------------------------------------------

double g_scale = 1.0;
bool g_smoke = false;
std::string g_filter;
std::vector<BenchResult> g_results;
std::vector<std::string> g_gate_failures;

bool Enabled(const std::string& name) {
  return g_filter.empty() || name.find(g_filter) != std::string::npos;
}

int ScaledSamples(int samples, int floor_samples = 50) {
  const int scaled = static_cast<int>(samples * g_scale);
  return scaled < floor_samples ? floor_samples : scaled;
}

Duration ScaledDuration(Duration duration) {
  const auto scaled =
      std::chrono::duration_cast<Duration>(duration * g_scale);
  return scaled < 500ms ? Duration(500ms) : scaled;
}

void Record(BenchResult result) {
  if (result.alloc_gated && result.allocations != 0) {
    g_gate_failures.push_back(result.name + ": " +
                              std::to_string(result.allocations) +
                              " allocation(s) in the measured section");
  }
  std::printf("%-44s p50 %9.1f  p99 %9.1f  p99.9 %9.1f  max %10.1f ns"
              "  (n=%zu%s, alloc=%llu%s)\n",
              result.name.c_str(), result.stats.p50, result.stats.p99,
              result.stats.p999, result.stats.max, result.stats.samples,
              result.batch > 1 ? ", batched" : "",
              static_cast<unsigned long long>(result.allocations),
              result.alloc_gated ? " gated" : "");
  if (result.has_jitter) {
    std::printf("%-44s jitter p99 %.1f ns  max %.1f ns\n", "",
                result.jitter_p99_ns, result.jitter_max_ns);
  }
  g_results.push_back(std::move(result));
}

// ---------------------------------------------------------------------------
// Micro measurement: batched steady-clock sampling under the alloc probe.
// Buffers are preallocated BEFORE the probe starts (bench-code exemption
// ends at the measured section).
// ---------------------------------------------------------------------------

template <typename Op>
Stats MeasureMicro(Op&& op, int batch, int samples,
                   std::uint64_t* allocations) {
  for (int i = 0; i < batch * 4; ++i) {
    op();  // warmup: fault pages, warm caches, settle the branch predictor
  }
  std::vector<double> per_op_ns(static_cast<std::size_t>(samples));
  {
    xmmsg_test::AllocProbe probe;
    for (int s = 0; s < samples; ++s) {
      const Timestamp t0 = Now();
      for (int i = 0; i < batch; ++i) {
        op();
      }
      const Timestamp t1 = Now();
      per_op_ns[static_cast<std::size_t>(s)] =
          std::chrono::duration<double, std::nano>(t1 - t0).count() / batch;
    }
    *allocations = probe.allocations();
  }
  return ComputeStats(std::move(per_op_ns));
}

// Variant with an UNPROBED, untimed per-sample setup (queue refill/drain).
template <typename Setup, typename Op>
Stats MeasureMicroWithSetup(Setup&& setup, Op&& op, int batch, int samples,
                            std::uint64_t* allocations) {
  setup();
  for (int i = 0; i < batch; ++i) {
    op();  // warmup
  }
  std::vector<double> per_op_ns(static_cast<std::size_t>(samples));
  std::uint64_t total_allocations = 0;
  for (int s = 0; s < samples; ++s) {
    setup();
    {
      xmmsg_test::AllocProbe probe;
      const Timestamp t0 = Now();
      for (int i = 0; i < batch; ++i) {
        op();
      }
      const Timestamp t1 = Now();
      per_op_ns[static_cast<std::size_t>(s)] =
          std::chrono::duration<double, std::nano>(t1 - t0).count() / batch;
      total_allocations += probe.allocations();
    }
  }
  *allocations = total_allocations;
  return ComputeStats(std::move(per_op_ns));
}

// Batch sizes: amortize the ~20 ns clock read below the op cost, small
// enough that batch means still expose tails (batch is recorded per row).
template <std::size_t kBytes>
constexpr int MicroBatch() {
  return kBytes >= 65536 ? 32 : 256;
}
template <std::size_t kBytes>
int MicroSamples() {
  return ScaledSamples(kBytes >= 65536 ? 300 : 400);
}

// ---------------------------------------------------------------------------
// Micro layer.
// ---------------------------------------------------------------------------

template <std::size_t kBytes>
void BenchPublishLatest(bool contended_readers = false) {
  BenchResult r;
  r.name = std::string(contended_readers ? "contended/publish_latest_1w4r/"
                                         : "micro/publish_latest/") +
           SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_pub_" + r.name});
  auto pub = domain.Advertise<Payload<kBytes>>(
      "bench.pub", {.history = msg::History::LatestOnly()});
  const int reader_count = contended_readers ? 4 : 1;
  std::vector<msg::Subscriber<Payload<kBytes>>> subs;
  subs.reserve(static_cast<std::size_t>(reader_count));
  for (int i = 0; i < reader_count; ++i) {
    subs.push_back(domain.Subscribe<Payload<kBytes>>(
        "bench.pub", {.history = msg::History::LatestOnly()}));
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  if (contended_readers) {
    for (auto& sub : subs) {
      readers.emplace_back([&stop, &sub] {
        while (!stop.load(std::memory_order_relaxed)) {
          auto sample = sub.TakeLatest();
          if (sample.freshness() != msg::Freshness::kNone) {
            g_sink = g_sink + sample->seq;
          }
        }
      });
    }
  }

  Payload<kBytes> value{};
  std::uint64_t seq = 0;
  r.stats = MeasureMicro(
      [&] {
        value.seq = ++seq;
        (void)pub.Publish(value);
      },
      MicroBatch<kBytes>(), MicroSamples<kBytes>(), &r.allocations);

  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }

  r.layer = contended_readers ? "contended" : "micro";
  r.payload_bytes = kBytes;
  r.contended = contended_readers;
  r.batch = MicroBatch<kBytes>();
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchLoanPublish() {
  BenchResult r;
  r.name = std::string("micro/loan_publish/") + SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_loan_" + r.name});
  auto pub = domain.Advertise<Payload<kBytes>>(
      "bench.loan", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<Payload<kBytes>>(
      "bench.loan", {.history = msg::History::LatestOnly()});
  (void)sub;

  std::uint64_t seq = 0;
  r.stats = MeasureMicro(
      [&] {
        auto loan = pub.Loan();
        loan->seq = ++seq;
        (void)pub.Publish(std::move(loan));
      },
      MicroBatch<kBytes>(), MicroSamples<kBytes>(), &r.allocations);

  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = MicroBatch<kBytes>();
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchTakeLatestHit(bool contended = false) {
  BenchResult r;
  r.name = std::string(contended ? "contended/take_latest_1w4r/"
                                 : "micro/take_latest_hit/") +
           SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_take_" + r.name});
  auto pub = domain.Advertise<Payload<kBytes>>(
      "bench.take", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<Payload<kBytes>>(
      "bench.take", {.history = msg::History::LatestOnly()});

  std::atomic<bool> stop{false};
  std::vector<std::thread> background;
  std::vector<msg::Subscriber<Payload<kBytes>>> peers;
  if (contended) {
    // Writer floods; 3 sibling readers spin; the 4th reader is measured.
    peers.reserve(3);
    for (int i = 0; i < 3; ++i) {
      peers.push_back(domain.Subscribe<Payload<kBytes>>(
          "bench.take", {.history = msg::History::LatestOnly()}));
    }
    background.emplace_back([&stop, &pub] {
      Payload<kBytes> value{};
      std::uint64_t seq = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        value.seq = ++seq;
        (void)pub.Publish(value);
      }
    });
    for (auto& peer : peers) {
      background.emplace_back([&stop, &peer] {
        while (!stop.load(std::memory_order_relaxed)) {
          auto sample = peer.TakeLatest();
          if (sample.freshness() != msg::Freshness::kNone) {
            g_sink = g_sink + sample->seq;
          }
        }
      });
    }
    // Ensure the slot has a value before measuring.
    while (sub.TakeLatest().freshness() == msg::Freshness::kNone) {
      std::this_thread::yield();
    }
  } else {
    Payload<kBytes> value{};
    value.seq = 1;
    (void)pub.Publish(value);
  }

  r.stats = MeasureMicro(
      [&] {
        auto sample = sub.TakeLatest();
        g_sink = g_sink + sample->seq;
      },
      MicroBatch<kBytes>(), MicroSamples<kBytes>(), &r.allocations);

  stop.store(true);
  for (auto& t : background) {
    t.join();
  }

  r.layer = contended ? "contended" : "micro";
  r.payload_bytes = kBytes;
  r.contended = contended;
  r.batch = MicroBatch<kBytes>();
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

void BenchTakeLatestMiss() {
  BenchResult r;
  r.name = "micro/take_latest_miss/64B";
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_take_miss"});
  auto pub = domain.Advertise<Payload<64>>(
      "bench.take_miss", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<Payload<64>>(
      "bench.take_miss", {.history = msg::History::LatestOnly()});
  (void)pub;  // matched but never publishes: every take is a kNone miss

  r.stats = MeasureMicro(
      [&] {
        auto sample = sub.TakeLatest();
        g_sink = g_sink + static_cast<std::uint64_t>(sample.freshness());
      },
      MicroBatch<64>(), MicroSamples<64>(), &r.allocations);

  r.layer = "micro";
  r.payload_bytes = 64;
  r.batch = MicroBatch<64>();
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

constexpr std::uint32_t kQueueDepth = 1024;
constexpr int kQueueBatch = 512;  // < depth: no overflow inside a sample

template <std::size_t kBytes>
void BenchQueuePublish() {
  BenchResult r;
  r.name = std::string("micro/queue_publish/") + SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_qpub_" + r.name});
  const msg::Qos qos{.history = msg::History::Queue(kQueueDepth)};
  auto pub = domain.Advertise<Payload<kBytes>>("bench.qpub", qos);
  auto sub = domain.Subscribe<Payload<kBytes>>("bench.qpub", qos);

  Payload<kBytes> value{};
  std::uint64_t seq = 0;
  r.stats = MeasureMicroWithSetup(
      [&] {  // drain (untimed) so each timed batch enqueues into headroom
        while (sub.TryTake().freshness() != msg::Freshness::kNone) {
        }
      },
      [&] {
        value.seq = ++seq;
        (void)pub.Publish(value);
      },
      kQueueBatch, ScaledSamples(kBytes >= 65536 ? 100 : 200),
      &r.allocations);

  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = kQueueBatch;
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchQueueTryTake() {
  BenchResult r;
  r.name = std::string("micro/queue_trytake/") + SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_qtake_" + r.name});
  const msg::Qos qos{.history = msg::History::Queue(kQueueDepth)};
  auto pub = domain.Advertise<Payload<kBytes>>("bench.qtake", qos);
  auto sub = domain.Subscribe<Payload<kBytes>>("bench.qtake", qos);

  Payload<kBytes> value{};
  std::uint64_t seq = 0;
  r.stats = MeasureMicroWithSetup(
      [&] {  // refill (untimed): drain leftovers, then enqueue one batch
        while (sub.TryTake().freshness() != msg::Freshness::kNone) {
        }
        for (int i = 0; i < kQueueBatch; ++i) {
          value.seq = ++seq;
          (void)pub.Publish(value);
        }
      },
      [&] {
        auto sample = sub.TryTake();
        if (sample.freshness() != msg::Freshness::kNone) {
          g_sink = g_sink + sample->seq;
        }
      },
      kQueueBatch, ScaledSamples(kBytes >= 65536 ? 100 : 200),
      &r.allocations);

  r.layer = "micro";
  r.payload_bytes = kBytes;
  r.batch = kQueueBatch;
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

// Contended queue variants (M9-A6): a real concurrent SPSC pair — the
// measured verb races its peer on the ring.
template <std::size_t kBytes>
void BenchQueueContended(bool measure_publish) {
  BenchResult r;
  r.name = std::string(measure_publish ? "contended/queue_publish_spsc_race/"
                                       : "contended/queue_trytake_spsc_race/") +
           SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_qrace_" + r.name});
  const msg::Qos qos{.history = msg::History::Queue(kQueueDepth)};
  auto pub = domain.Advertise<Payload<kBytes>>("bench.qrace", qos);
  auto sub = domain.Subscribe<Payload<kBytes>>("bench.qrace", qos);

  std::atomic<bool> stop{false};
  std::thread peer;
  if (measure_publish) {
    peer = std::thread([&] {  // consumer drains continuously
      while (!stop.load(std::memory_order_relaxed)) {
        auto sample = sub.TryTake();
        if (sample.freshness() != msg::Freshness::kNone) {
          g_sink = g_sink + sample->seq;
        }
      }
    });
    Payload<kBytes> value{};
    std::uint64_t seq = 0;
    r.stats = MeasureMicro(
        [&] {
          value.seq = ++seq;
          (void)pub.Publish(value);
        },
        MicroBatch<kBytes>(), ScaledSamples(300), &r.allocations);
  } else {
    peer = std::thread([&] {  // producer floods continuously
      Payload<kBytes> value{};
      std::uint64_t seq = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        value.seq = ++seq;
        (void)pub.Publish(value);
      }
    });
    r.stats = MeasureMicro(
        [&] {
          auto sample = sub.TryTake();
          if (sample.freshness() != msg::Freshness::kNone) {
            g_sink = g_sink + sample->seq;
          }
        },
        MicroBatch<kBytes>(), ScaledSamples(300), &r.allocations);
  }
  stop.store(true);
  peer.join();

  r.layer = "contended";
  r.payload_bytes = kBytes;
  r.contended = true;
  r.batch = MicroBatch<kBytes>();
  r.alloc_gated = true;  // M9-A3
  Record(std::move(r));
}

template <std::size_t kBytes>
void BenchRpcRoundTrip(bool contended_clients = false) {
  BenchResult r;
  r.name = std::string(contended_clients ? "contended/rpc_call_4clients/"
                                         : "micro/rpc_call_roundtrip/") +
           SizeLabel(kBytes);
  if (!Enabled(r.name)) {
    return;
  }
  auto domain = msg::Domain::InProcess({.name = "bench_rpc_" + r.name});
  auto server = domain.Serve<Payload<kBytes>, Payload<kBytes>>("bench.rpc");
  auto client = domain.Client<Payload<kBytes>, Payload<kBytes>>("bench.rpc");

  std::atomic<bool> stop{false};
  std::thread server_thread([&] {
    Payload<kBytes> response{};
    while (!stop.load(std::memory_order_relaxed)) {
      auto request = server.TakeRequest();
      if (request.freshness() != msg::Freshness::kNone) {
        response.seq = request->seq;
        (void)server.Reply(request, response);
      } else {
        (void)server.WaitForWorkOrShutdown(1ms);
      }
    }
  });

  std::vector<std::thread> extra_clients;
  std::vector<msg::Client<Payload<kBytes>, Payload<kBytes>>> peers;
  if (contended_clients) {
    peers.reserve(3);
    for (int i = 0; i < 3; ++i) {
      peers.push_back(
          domain.Client<Payload<kBytes>, Payload<kBytes>>("bench.rpc"));
    }
    for (auto& peer : peers) {
      extra_clients.emplace_back([&stop, &peer] {
        Payload<kBytes> request{};
        while (!stop.load(std::memory_order_relaxed)) {
          request.seq += 1;
          (void)peer.Call(request, 100ms);
        }
      });
    }
  }

  Payload<kBytes> request{};
  std::uint64_t seq = 0;
  const int samples =
      ScaledSamples(contended_clients ? 1500 : (kBytes >= 65536 ? 1000 : 2500));
  r.stats = MeasureMicro(
      [&] {
        request.seq = ++seq;
        auto result = client.Call(request, 100ms);
        if (result.status() == msg::CallStatus::kOk) {
          g_sink = g_sink + result->seq;
        }
      },
      /*batch=*/1, samples, &r.allocations);

  stop.store(true);
  server_thread.join();
  for (auto& t : extra_clients) {
    t.join();
  }

  r.layer = contended_clients ? "contended" : "micro";
  r.payload_bytes = kBytes;
  r.contended = contended_clients;
  r.batch = 1;
  // Recorded, not gated: Call is a blocking rendezvous verb — the R7/M9-A3
  // allocation-free obligation covers the publish/take hot path.
  r.alloc_gated = false;
  Record(std::move(r));
}

// ---------------------------------------------------------------------------
// Path layer: single-hop latency tails, per event (batch = 1), measured as
// take-time minus the library's publish stamp (one monotonic clock, R8).
// ---------------------------------------------------------------------------

struct PathOutcome {
  Stats latency;
  bool has_jitter = false;
  double jitter_p99_ns = 0.0;
  double jitter_max_ns = 0.0;
  std::uint64_t producer_allocations = 0;
  std::uint64_t consumer_allocations = 0;
};

template <std::size_t kBytes>
PathOutcome RunPath(const std::string& isolation, Duration period,
                    Duration run_for, std::size_t max_samples) {
  auto domain = msg::Domain::InProcess({.name = isolation});
  auto pub = domain.Advertise<Payload<kBytes>>(
      "bench.path", {.history = msg::History::LatestOnly()});
  auto sub = domain.Subscribe<Payload<kBytes>>(
      "bench.path", {.history = msg::History::LatestOnly()});

  PathOutcome outcome;
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    Payload<kBytes> value{};
    std::uint64_t seq = 0;
    const Timestamp end_at = Now() + run_for;
    Timestamp next_tick = Now();
    xmmsg_test::AllocProbe probe;  // gates Publish on this thread (M9-A3)
    while (Now() < end_at) {
      if (period > Duration::zero()) {
        next_tick += period;
        std::this_thread::sleep_until(next_tick);
      }
      value.seq = ++seq;
      (void)pub.Publish(value);
    }
    outcome.producer_allocations = probe.allocations();
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<double> latency_ns;
  std::vector<double> arrival_ns;
  latency_ns.reserve(max_samples);
  arrival_ns.reserve(max_samples);
  {
    xmmsg_test::AllocProbe probe;  // gates TakeLatest on this thread
    std::uint64_t last_seq = 0;
    Timestamp last_arrival{};
    while (!producer_done.load(std::memory_order_acquire)) {
      auto sample = sub.TakeLatest();
      if (sample.freshness() == msg::Freshness::kNone ||
          sample->seq == last_seq) {
        continue;
      }
      const Timestamp now = Now();
      last_seq = sample->seq;
      if (latency_ns.size() < max_samples) {
        latency_ns.push_back(
            std::chrono::duration<double, std::nano>(now - sample.stamp())
                .count());
      }
      if (period > Duration::zero()) {
        if (last_arrival.time_since_epoch().count() != 0 &&
            arrival_ns.size() < max_samples) {
          arrival_ns.push_back(
              std::chrono::duration<double, std::nano>(now - last_arrival)
                  .count());
        }
        last_arrival = now;
      }
    }
    outcome.consumer_allocations = probe.allocations();
  }
  producer.join();

  outcome.latency = ComputeStats(std::move(latency_ns));
  if (period > Duration::zero() && !arrival_ns.empty()) {
    const double period_ns =
        std::chrono::duration<double, std::nano>(period).count();
    std::vector<double> jitter(arrival_ns.size());
    for (std::size_t i = 0; i < arrival_ns.size(); ++i) {
      jitter[i] = std::abs(arrival_ns[i] - period_ns);
    }
    const Stats jitter_stats = ComputeStats(std::move(jitter));
    outcome.has_jitter = true;
    outcome.jitter_p99_ns = jitter_stats.p99;
    outcome.jitter_max_ns = jitter_stats.max;
  }
  return outcome;
}

void RecordPath(const std::string& name, std::size_t payload_bytes,
                bool contended, const PathOutcome& outcome,
                const std::string& notes = {}) {
  BenchResult r;
  r.name = name;
  r.layer = contended ? "contended" : "path";
  r.payload_bytes = payload_bytes;
  r.contended = contended;
  r.batch = 1;
  r.stats = outcome.latency;
  r.allocations =
      outcome.producer_allocations + outcome.consumer_allocations;
  r.alloc_gated = true;  // both loop bodies are publish/take (M9-A3)
  r.has_jitter = outcome.has_jitter;
  r.jitter_p99_ns = outcome.jitter_p99_ns;
  r.jitter_max_ns = outcome.jitter_max_ns;
  r.notes = notes;
  Record(std::move(r));
}

void BenchPathLayer() {
  // M1 profile: 10 Hz plan-sized hop + 1 kHz control-sized hop (R4).
  if (Enabled("path/hop_latency_10hz/1KiB")) {
    const auto outcome = RunPath<1024>("bench_path_10hz", 100ms,
                                       ScaledDuration(3s), 4096);
    RecordPath("path/hop_latency_10hz/1KiB", 1024, false, outcome,
               "M1 plan leg; low sample count is inherent at 10 Hz");
  }
  if (Enabled("path/hop_latency_1khz/64B")) {
    const auto outcome =
        RunPath<64>("bench_path_1khz", 1ms, ScaledDuration(4s), 65536);
    RecordPath("path/hop_latency_1khz/64B", 64, false, outcome,
               "M1 control leg");
  }
  if (Enabled("path/hop_latency_saturation/64B")) {
    const auto outcome = RunPath<64>("bench_path_sat", Duration::zero(),
                                     ScaledDuration(2s), 400000);
    RecordPath("path/hop_latency_saturation/64B", 64, false, outcome,
               "unpaced writer; latest-only overwrites are expected");
  }
}

// ---------------------------------------------------------------------------
// System layer (R4): the robot-typical profile — 30 topics, mixed rates,
// several publishers and subscribers, per-topic hop tails under combined
// load. 64 B payloads (small, robot-typical).
// ---------------------------------------------------------------------------

struct RateClass {
  const char* label;
  Duration period;
  int topics;
};

void BenchSystemRobotProfile() {
  if (!Enabled("system/robot30")) {
    return;
  }
  constexpr RateClass kClasses[] = {
      {"1khz", 1ms, 6}, {"100hz", 10ms, 8}, {"10hz", 100ms, 16}};
  const Duration run_for = ScaledDuration(5s);

  auto domain = msg::Domain::InProcess({.name = "bench_system_robot30"});

  struct Topic {
    msg::Publisher<Payload<64>> pub;
    msg::Subscriber<Payload<64>> sub;
    std::vector<double> latency_ns;
    std::uint64_t last_seq = 0;
  };

  // Wire everything up front (wiring is not the hot path).
  std::vector<std::vector<Topic>> classes;
  classes.reserve(3);
  for (const RateClass& rc : kClasses) {
    std::vector<Topic> topics;
    topics.reserve(static_cast<std::size_t>(rc.topics));
    const auto expected = static_cast<std::size_t>(
        run_for / rc.period + 64);
    for (int i = 0; i < rc.topics; ++i) {
      char name[64];
      std::snprintf(name, sizeof(name), "bench.sys.%s.t%02d", rc.label, i);
      Topic topic{domain.Advertise<Payload<64>>(
                      name, {.history = msg::History::LatestOnly()}),
                  domain.Subscribe<Payload<64>>(
                      name, {.history = msg::History::LatestOnly()}),
                  {},
                  0};
      topic.latency_ns.reserve(expected);
      topics.push_back(std::move(topic));
    }
    classes.push_back(std::move(topics));
  }

  // One producer thread and one consumer thread per rate class: 6 threads,
  // 30 concurrent topics — the combined load R4 asks for.
  std::atomic<bool> producers_done{false};
  std::atomic<std::uint64_t> producer_allocations{0};
  std::atomic<std::uint64_t> consumer_allocations{0};
  std::vector<std::thread> threads;
  std::atomic<int> producers_running{3};

  for (std::size_t c = 0; c < 3; ++c) {
    // Index-based access inside the lambdas: loop-local references must not
    // be captured by reference across iterations (they die with the scope).
    threads.emplace_back([&, c] {
      const RateClass& rc = kClasses[c];
      auto& topics = classes[c];
      Payload<64> value{};
      std::uint64_t seq = 0;
      const Timestamp end_at = Now() + run_for;
      Timestamp next_tick = Now();
      xmmsg_test::AllocProbe probe;  // gates Publish (M9-A3)
      while (Now() < end_at) {
        next_tick += rc.period;
        std::this_thread::sleep_until(next_tick);
        value.seq = ++seq;
        for (Topic& topic : topics) {
          (void)topic.pub.Publish(value);
        }
      }
      producer_allocations.fetch_add(probe.allocations(),
                                     std::memory_order_relaxed);
      if (producers_running.fetch_sub(1) == 1) {
        producers_done.store(true, std::memory_order_release);
      }
    });
    threads.emplace_back([&, c] {
      auto& my_topics = classes[c];
      xmmsg_test::AllocProbe probe;  // gates TakeLatest (M9-A3)
      while (!producers_done.load(std::memory_order_acquire)) {
        for (Topic& topic : my_topics) {
          auto sample = topic.sub.TakeLatest();
          if (sample.freshness() == msg::Freshness::kNone ||
              sample->seq == topic.last_seq) {
            continue;
          }
          topic.last_seq = sample->seq;
          if (topic.latency_ns.size() < topic.latency_ns.capacity()) {
            topic.latency_ns.push_back(std::chrono::duration<double, std::nano>(
                                           Now() - sample.stamp())
                                           .count());
          }
        }
      }
      consumer_allocations.fetch_add(probe.allocations(),
                                     std::memory_order_relaxed);
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  // Per-topic rows (M9: per-topic hop tails under the combined load) plus a
  // per-class aggregate row for the human summary and regression gate.
  const std::uint64_t allocations =
      producer_allocations.load() + consumer_allocations.load();
  for (std::size_t c = 0; c < 3; ++c) {
    const RateClass& rc = kClasses[c];
    std::vector<double> class_all;
    for (std::size_t i = 0; i < classes[c].size(); ++i) {
      Topic& topic = classes[c][i];
      class_all.insert(class_all.end(), topic.latency_ns.begin(),
                       topic.latency_ns.end());
      BenchResult row;
      char name[96];
      std::snprintf(name, sizeof(name), "system/robot30/%s/t%02zu/64B",
                    rc.label, i);
      row.name = name;
      row.layer = "system";
      row.payload_bytes = 64;
      row.batch = 1;
      row.stats = ComputeStats(std::move(topic.latency_ns));
      row.alloc_gated = false;  // gated once on the aggregate row below
      g_results.push_back(std::move(row));  // per-topic rows: JSON only
    }
    BenchResult aggregate;
    aggregate.name = std::string("system/robot30/") + rc.label + "_class/64B";
    aggregate.layer = "system";
    aggregate.payload_bytes = 64;
    aggregate.batch = 1;
    aggregate.stats = ComputeStats(std::move(class_all));
    aggregate.allocations = c == 0 ? allocations : 0;  // report once
    aggregate.alloc_gated = c == 0;                    // gate once (M9-A3)
    aggregate.notes = std::to_string(rc.topics) + " topics under the "
                      "combined 30-topic load";
    Record(std::move(aggregate));
  }
}

// ---------------------------------------------------------------------------
// Contended path (M9-A6): the M3 flood profile running concurrently with a
// measured 1 kHz control topic.
// ---------------------------------------------------------------------------

void BenchControlUnderFlood() {
  if (!Enabled("contended/control_hop_1khz_under_flood/64B")) {
    return;
  }
  std::atomic<bool> stop{false};
  auto flood_domain = msg::Domain::InProcess({.name = "bench_flood"});

  // Flood pair 1 (M3 shape): queue(8), best-effort, consumer stalled — the
  // publisher hammers the overflow/drop path continuously.
  auto flood_qpub = flood_domain.Advertise<Payload<64>>(
      "bench.flood.q", {.history = msg::History::Queue(8)});
  auto flood_qsub = flood_domain.Subscribe<Payload<64>>(
      "bench.flood.q", {.history = msg::History::Queue(8)});
  (void)flood_qsub;  // stalled: never takes

  // Flood pair 2: latest-only writer + spinning reader at full rate.
  auto flood_lpub = flood_domain.Advertise<Payload<64>>(
      "bench.flood.l", {.history = msg::History::LatestOnly()});
  auto flood_lsub = flood_domain.Subscribe<Payload<64>>(
      "bench.flood.l", {.history = msg::History::LatestOnly()});

  std::vector<std::thread> flood_threads;
  flood_threads.emplace_back([&] {
    Payload<64> value{};
    std::uint64_t seq = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      value.seq = ++seq;
      (void)flood_qpub.Publish(value);
    }
  });
  flood_threads.emplace_back([&] {
    Payload<64> value{};
    std::uint64_t seq = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      value.seq = ++seq;
      (void)flood_lpub.Publish(value);
    }
  });
  flood_threads.emplace_back([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      auto sample = flood_lsub.TakeLatest();
      if (sample.freshness() != msg::Freshness::kNone) {
        g_sink = g_sink + sample->seq;
      }
    }
  });

  const auto outcome = RunPath<64>("bench_control_under_flood", 1ms,
                                   ScaledDuration(4s), 65536);
  stop.store(true);
  for (auto& t : flood_threads) {
    t.join();
  }
  RecordPath("contended/control_hop_1khz_under_flood/64B", 64, true, outcome,
             "M3 flood profile (queue overflow + latest flood) concurrent; "
             "compare path/hop_latency_1khz/64B");
}

// ---------------------------------------------------------------------------
// Human summary + entry point.
// ---------------------------------------------------------------------------

void PrintHardwareContext(const xmmsg_bench::HardwareContext& hw) {
  std::printf("xmMessaging M9 bench — in-process reach (P0b reference)\n");
  std::printf("  cpu      : %s (%u hw threads)\n", hw.cpu_model.c_str(),
              hw.nproc);
  std::printf("  governor : %s%s\n", hw.governor.c_str(),
              hw.governor == "performance"
                  ? ""
                  : "  [not 'performance' — expect noisier tails]");
  std::printf("  kernel   : %s%s\n", hw.kernel.c_str(),
              hw.preempt_rt ? " (PREEMPT_RT)" : " (no RT patch)");
  std::printf("  load avg : %.2f %.2f %.2f\n", hw.load_avg_1m, hw.load_avg_5m,
              hw.load_avg_15m);
  std::printf("  scale    : %.3f%s\n\n", g_scale, g_smoke ? " (smoke)" : "");
}

int Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--out <report.json>] [--smoke] [--filter <substr>]\n",
               argv0);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--smoke") {
      g_smoke = true;
      g_scale = 0.15;
    } else if (arg == "--filter" && i + 1 < argc) {
      g_filter = argv[++i];
    } else {
      return Usage(argv[0]);
    }
  }

  const auto hw = xmmsg_bench::CaptureHardwareContext();
  PrintHardwareContext(hw);

  // --- micro layer ----------------------------------------------------------
  BenchPublishLatest<64>();
  BenchPublishLatest<1024>();
  BenchPublishLatest<65536>();
  BenchLoanPublish<64>();
  BenchLoanPublish<1024>();
  BenchLoanPublish<65536>();
  BenchTakeLatestHit<64>();
  BenchTakeLatestHit<1024>();
  BenchTakeLatestHit<65536>();
  BenchTakeLatestMiss();
  BenchQueuePublish<64>();
  BenchQueuePublish<1024>();
  BenchQueuePublish<65536>();
  BenchQueueTryTake<64>();
  BenchQueueTryTake<1024>();
  BenchQueueTryTake<65536>();
  BenchRpcRoundTrip<64>();
  BenchRpcRoundTrip<1024>();
  BenchRpcRoundTrip<65536>();

  // --- path layer -----------------------------------------------------------
  BenchPathLayer();

  // --- system layer (R4 robot-typical profile) -------------------------------
  BenchSystemRobotProfile();

  // --- contended variants (M9-A6) --------------------------------------------
  BenchPublishLatest<64>(/*contended_readers=*/true);
  BenchPublishLatest<65536>(/*contended_readers=*/true);
  BenchTakeLatestHit<64>(/*contended=*/true);
  BenchTakeLatestHit<65536>(/*contended=*/true);
  BenchQueueContended<64>(/*measure_publish=*/true);
  BenchQueueContended<64>(/*measure_publish=*/false);
  BenchRpcRoundTrip<64>(/*contended_clients=*/true);
  BenchControlUnderFlood();

  // --- report ----------------------------------------------------------------
  if (!out_path.empty()) {
    if (!xmmsg_bench::WriteJsonReport(out_path, hw, g_results, g_smoke,
                                      g_scale, g_gate_failures)) {
      std::fprintf(stderr, "xmmsg_bench: FAILED to write %s\n",
                   out_path.c_str());
      return 1;
    }
    std::printf("\nreport: %s (%zu benchmarks)\n", out_path.c_str(),
                g_results.size());
  }

  if (!g_gate_failures.empty()) {
    std::fprintf(stderr, "\nM9-A3 ALLOC GATE FAILED:\n");
    for (const std::string& failure : g_gate_failures) {
      std::fprintf(stderr, "  %s\n", failure.c_str());
    }
    return 1;
  }
  std::printf("M9-A3 alloc gate: PASSED (0 allocations in every gated "
              "measured section)\n");
  return 0;
}

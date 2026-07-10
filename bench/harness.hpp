/*
 * bench/harness.hpp — the M9 measurement harness (hand-rolled, R1).
 *
 * No google-benchmark: the family vendors none (telemetry's perf tier is the
 * same hand-rolled shape — batched steady_clock sampling, percentile math),
 * and a benchmark dependency would violate the no-new-external-deps rule for
 * a job this small. What R4 actually requires is here: tails (p50/p99/p99.9/
 * max, never means alone), hardware context embedded in every report, and a
 * machine-readable JSON artifact.
 *
 * Percentiles use the nearest-rank method on the sorted sample vector.
 * Micro layers batch back-to-back ops per timed sample (amortizing the
 * ~20 ns clock read below the cost of the measured op — the telemetry
 * perf_budgets.cpp / ros2_tracing methodology); the batch size is recorded
 * in the report because a batched "max" is a max of batch MEANS. Path and
 * system layers time individual message hops, so their tails are true
 * per-event tails.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace xmmsg_bench {

// ---------------------------------------------------------------------------
// Statistics: tails, not means (R4).
// ---------------------------------------------------------------------------

struct Stats {
  double p50 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::size_t samples = 0;
};

// Nearest-rank percentile over a SORTED vector.
inline double PercentileSorted(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return 0.0;
  }
  const auto rank = static_cast<std::size_t>(
      std::ceil(q * static_cast<double>(sorted.size())));
  const std::size_t index = rank == 0 ? 0 : rank - 1;
  return sorted[std::min(index, sorted.size() - 1)];
}

inline Stats ComputeStats(std::vector<double> samples) {
  Stats stats;
  stats.samples = samples.size();
  if (samples.empty()) {
    return stats;
  }
  std::sort(samples.begin(), samples.end());
  double sum = 0.0;
  for (double v : samples) {
    sum += v;
  }
  stats.p50 = PercentileSorted(samples, 0.50);
  stats.p99 = PercentileSorted(samples, 0.99);
  stats.p999 = PercentileSorted(samples, 0.999);
  stats.max = samples.back();
  stats.mean = sum / static_cast<double>(samples.size());
  return stats;
}

// ---------------------------------------------------------------------------
// One benchmark's report row.
// ---------------------------------------------------------------------------

struct BenchResult {
  std::string name;          // e.g. "micro/publish_latest/64B"
  std::string layer;         // "micro" | "path" | "system" | "contended"
  std::size_t payload_bytes = 0;
  bool contended = false;
  int batch = 1;             // ops per timed sample (1 = true per-event tails)
  Stats stats;               // nanoseconds
  std::uint64_t allocations = 0;  // measured-section allocations (probe)
  bool alloc_gated = false;  // true: allocations != 0 fails the run (M9-A3)
  bool has_jitter = false;   // paced path profiles only
  double jitter_p99_ns = 0.0;  // p99 of |inter-arrival - period|
  double jitter_max_ns = 0.0;
  std::string notes;
};

// ---------------------------------------------------------------------------
// Hardware context (M9-A1: a context-less report is a failing run).
// ---------------------------------------------------------------------------

struct HardwareContext {
  std::string cpu_model = "unknown";
  std::string governor = "unknown";
  std::string kernel = "unknown";
  bool preempt_rt = false;
  unsigned nproc = 0;
  double load_avg_1m = -1.0;
  double load_avg_5m = -1.0;
  double load_avg_15m = -1.0;
};

inline std::string ReadFirstLine(const char* path) {
  std::ifstream in(path);
  std::string line;
  if (in && std::getline(in, line)) {
    return line;
  }
  return {};
}

inline HardwareContext CaptureHardwareContext() {
  HardwareContext hw;

  // CPU model: first "model name" row of /proc/cpuinfo.
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (cpuinfo && std::getline(cpuinfo, line)) {
      if (line.rfind("model name", 0) == 0) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
          auto value = line.substr(colon + 1);
          const auto first = value.find_first_not_of(" \t");
          hw.cpu_model = first == std::string::npos ? value : value.substr(first);
        }
        break;
      }
    }
  }

  const std::string governor =
      ReadFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  if (!governor.empty()) {
    hw.governor = governor;
  }

  struct utsname uts {};
  if (::uname(&uts) == 0) {
    hw.kernel = uts.release;
    hw.preempt_rt = std::string(uts.version).find("PREEMPT_RT") !=
                    std::string::npos;
  }
  // PREEMPT_RT also exposes /sys/kernel/realtime == "1".
  if (ReadFirstLine("/sys/kernel/realtime") == "1") {
    hw.preempt_rt = true;
  }

  hw.nproc = std::thread::hardware_concurrency();

  {
    std::ifstream loadavg("/proc/loadavg");
    if (loadavg) {
      loadavg >> hw.load_avg_1m >> hw.load_avg_5m >> hw.load_avg_15m;
    }
  }
  return hw;
}

// ---------------------------------------------------------------------------
// JSON report writer (M9-A1). Hand-written emitter: the schema is small,
// fixed, and first-party — a JSON library would be a dependency for nothing.
// ---------------------------------------------------------------------------

inline void JsonEscapeTo(std::string& out, const std::string& value) {
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) {
          out += c;
        }
        break;
    }
  }
}

inline std::string UtcTimestamp() {
  char buffer[32] = {};
  const std::time_t now = std::time(nullptr);
  std::tm tm_utc{};
  gmtime_r(&now, &tm_utc);
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buffer;
}

inline bool WriteJsonReport(const std::string& path, const HardwareContext& hw,
                            const std::vector<BenchResult>& results,
                            bool smoke, double scale,
                            const std::vector<std::string>& gate_failures) {
  std::string out;
  out.reserve(64 * 1024);
  char num[64];

  const auto field_str = [&](const char* key, const std::string& value,
                             bool trailing_comma = true) {
    out += '"';
    out += key;
    out += "\": \"";
    JsonEscapeTo(out, value);
    out += '"';
    if (trailing_comma) {
      out += ", ";
    }
  };
  const auto field_num = [&](const char* key, double value,
                             bool trailing_comma = true) {
    std::snprintf(num, sizeof(num), "%.1f", value);
    out += '"';
    out += key;
    out += "\": ";
    out += num;
    if (trailing_comma) {
      out += ", ";
    }
  };
  const auto field_int = [&](const char* key, long long value,
                             bool trailing_comma = true) {
    std::snprintf(num, sizeof(num), "%lld", value);
    out += '"';
    out += key;
    out += "\": ";
    out += num;
    if (trailing_comma) {
      out += ", ";
    }
  };
  const auto field_bool = [&](const char* key, bool value,
                              bool trailing_comma = true) {
    out += '"';
    out += key;
    out += "\": ";
    out += value ? "true" : "false";
    if (trailing_comma) {
      out += ", ";
    }
  };

  out += "{\n  ";
  field_str("schema", "xmmessaging-bench-v1");
  field_str("generated_at_utc", UtcTimestamp());
  field_bool("smoke", smoke);
  field_num("scale", scale, false);
  out += ",\n  \"hardware\": { ";
  field_str("cpu_model", hw.cpu_model);
  field_int("nproc", hw.nproc);
  field_str("governor", hw.governor);
  field_str("kernel", hw.kernel);
  field_bool("preempt_rt", hw.preempt_rt);
  field_num("load_avg_1m", hw.load_avg_1m);
  field_num("load_avg_5m", hw.load_avg_5m);
  field_num("load_avg_15m", hw.load_avg_15m, false);
  out += " },\n  \"alloc_gate\": { ";
  field_bool("passed", gate_failures.empty());
  out += "\"failures\": [";
  for (std::size_t i = 0; i < gate_failures.size(); ++i) {
    out += i == 0 ? "\"" : ", \"";
    JsonEscapeTo(out, gate_failures[i]);
    out += '"';
  }
  out += "] },\n  \"benchmarks\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const BenchResult& r = results[i];
    out += "    { ";
    field_str("name", r.name);
    field_str("layer", r.layer);
    field_int("payload_bytes", static_cast<long long>(r.payload_bytes));
    field_bool("contended", r.contended);
    field_str("unit", "ns");
    field_int("batch", r.batch);
    field_int("samples", static_cast<long long>(r.stats.samples));
    field_num("p50", r.stats.p50);
    field_num("p99", r.stats.p99);
    field_num("p999", r.stats.p999);
    field_num("max", r.stats.max);
    field_num("mean", r.stats.mean);
    field_int("allocations", static_cast<long long>(r.allocations));
    field_bool("alloc_gated", r.alloc_gated, r.has_jitter || !r.notes.empty());
    if (r.has_jitter) {
      field_num("jitter_p99", r.jitter_p99_ns, true);
      field_num("jitter_max", r.jitter_max_ns, !r.notes.empty());
    }
    if (!r.notes.empty()) {
      field_str("notes", r.notes, false);
    }
    out += i + 1 < results.size() ? " },\n" : " }\n";
  }
  out += "  ]\n}\n";

  std::FILE* file = std::fopen(path.c_str(), "w");
  if (file == nullptr) {
    return false;
  }
  const bool ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
  return std::fclose(file) == 0 && ok;
}

}  // namespace xmmsg_bench

/*
 * test/behavioral/capture_binding.hpp — metric-capture telemetry binding
 * (M13-A4).
 *
 * The lightest capture sink that can prove the R11/§7 instrument story:
 * a test-local xmBase telemetry Binding whose registration entries record
 * every instrument (name -> kind) and hand back real, test-readable slots.
 * This is deliberately NOT the xmTelemetry SDK capture sink — binding the
 * full SDK into xmMessaging's test tree would add a dependency this library
 * does not have (xmBase is the only family dependency, M8-A1); the xmBase
 * Binding seam is sufficient for what M13-A4 asserts at the metric plane:
 * presence, instrument kind, and value reconciliation.
 *
 * Slot lifetime: the binding contract (xmbase/telemetry/binding.hpp) says
 * slot memory is process-lifetime and is NEVER freed — endpoint handles
 * captured at registration keep writing after any rebind. The registry
 * below therefore leaks its slots intentionally.
 *
 * Install BEFORE wiring the endpoints under test: handles acquired while a
 * different (or no) binding was active point at that binding's slots
 * permanently (the documented xmBase contract).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "xmbase/telemetry/binding.hpp"

namespace xmmsg_test {

enum class InstrumentKind { kCounter, kGauge, kHistogram };

// Name-keyed registry of captured instruments. A name registered twice
// (e.g. two subscribers on one topic, or two tests reusing a topic name)
// shares one slot — which is why value assertions in tests use unique
// topic names.
class MetricCapture {
 public:
  static MetricCapture& Instance() {
    static auto* capture = new MetricCapture();  // leaked: process-lifetime
    return *capture;
  }

  // Install the capture binding (idempotent). Explicit install disables
  // xmBase's console auto-adoption for the rest of the process — fine for
  // a test binary.
  void Install() {
    static const bool installed = [] {
      const bool ok = ::xmotion::telemetry::InstallBinding(&Binding());
      return ok;
    }();
    (void)installed;
  }

  bool Has(const std::string& name, InstrumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = kinds_.find(name);
    return it != kinds_.end() && it->second == kind;
  }

  double CounterValue(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(name);
    return it != counters_.end()
               ? it->second->value.load(std::memory_order_relaxed)
               : -1.0;
  }

  double GaugeValue(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(name);
    return it != gauges_.end()
               ? it->second->value.load(std::memory_order_relaxed)
               : -1.0;
  }

  std::uint64_t HistogramCount(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(name);
    return it != histograms_.end()
               ? it->second->count.load(std::memory_order_relaxed)
               : 0;
  }

  double HistogramMax(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(name);
    return it != histograms_.end()
               ? it->second->max.load(std::memory_order_relaxed)
               : -1.0;
  }

 private:
  MetricCapture() = default;

  static ::xmotion::telemetry::Binding& Binding() {
    using namespace ::xmotion::telemetry;
    static ::xmotion::telemetry::Binding binding = [] {
      ::xmotion::telemetry::Binding b{};
      b.abi_version = kBindingAbiVersion;
      b.get_counter = &GetCounterSlot;
      b.get_gauge = &GetGaugeSlot;
      b.get_histogram = &GetHistogramSlot;
      b.get_signal = &GetSignalSlot;
      b.intern_source = [](std::string_view) -> std::uint32_t { return 1; };
      b.should_log = [](Severity) noexcept { return false; };
      b.set_level = [](Severity) noexcept {};
      b.get_level = []() noexcept { return Severity::kOff; };
      b.emit_event = [](std::uint32_t, Severity, const char*,
                        const detail::ArgPack&, Context,
                        Timestamp) noexcept {};
      b.emit_event_dyn = [](std::uint32_t, Severity, const char*, std::size_t,
                            Context, Timestamp) noexcept {};
      b.emit_span = [](const char*, Context, SpanId, Timestamp, Timestamp,
                       const Context*, std::uint8_t) noexcept {};
      b.emit_signal = [](detail::SignalSlot*, const void*, std::size_t,
                         Timestamp) noexcept {};
      b.report_health = [](const char*, HealthState, const char*, Context,
                           Timestamp) noexcept {};
      b.set_resource = [](std::string_view, std::string_view) {};
      return b;
    }();
    return binding;
  }

  static ::xmotion::telemetry::detail::CounterSlot* GetCounterSlot(
      std::string_view name) {
    auto& self = Instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto& slot = self.counters_[std::string(name)];
    if (slot == nullptr) {
      slot = new ::xmotion::telemetry::detail::CounterSlot();  // leaked
      self.kinds_[std::string(name)] = InstrumentKind::kCounter;
    }
    return slot;
  }

  static ::xmotion::telemetry::detail::GaugeSlot* GetGaugeSlot(
      std::string_view name) {
    auto& self = Instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto& slot = self.gauges_[std::string(name)];
    if (slot == nullptr) {
      slot = new ::xmotion::telemetry::detail::GaugeSlot();  // leaked
      self.kinds_[std::string(name)] = InstrumentKind::kGauge;
    }
    return slot;
  }

  static ::xmotion::telemetry::detail::HistogramSlot* GetHistogramSlot(
      std::string_view name) {
    auto& self = Instance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    auto& slot = self.histograms_[std::string(name)];
    if (slot == nullptr) {
      slot = new ::xmotion::telemetry::detail::HistogramSlot();  // leaked
      self.kinds_[std::string(name)] = InstrumentKind::kHistogram;
    }
    return slot;
  }

  static ::xmotion::telemetry::detail::SignalSlot* GetSignalSlot(
      std::string_view, std::size_t, std::size_t, const char*) {
    static ::xmotion::telemetry::detail::SignalSlot slot;
    return &slot;
  }

  std::mutex mutex_;
  std::map<std::string, InstrumentKind> kinds_;
  std::map<std::string, ::xmotion::telemetry::detail::CounterSlot*> counters_;
  std::map<std::string, ::xmotion::telemetry::detail::GaugeSlot*> gauges_;
  std::map<std::string, ::xmotion::telemetry::detail::HistogramSlot*>
      histograms_;
};

}  // namespace xmmsg_test

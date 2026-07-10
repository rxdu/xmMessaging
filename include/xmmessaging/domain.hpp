/*
 * domain.hpp
 *
 * Domain — the application's messaging session. It owns the reach
 * configuration (selected at wiring time by choosing a factory, D12) and
 * every endpoint created from it. One API, three reaches: the call sites
 * on the endpoints never change; only the factory differs (M6-A1).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "xmmessaging/endpoints.hpp"
#include "xmmessaging/qos.hpp"
#include "xmmessaging/status.hpp"

namespace xmotion {
namespace messaging {

namespace detail {
class DomainImpl;
}  // namespace detail

// R8: the application's declaration of cross-host clock discipline. Ages
// from remote publishers are advisory (AgeClass::kAdvisory, no deadline
// verdicts) unless a synchronized domain is declared; the declaration is
// recorded in the envelope's home segment and introspection, so post-hoc
// analysis knows what the numbers meant.
enum class ClockDomain : std::uint8_t {
  kUnsynced,   // default: cross-host age is advisory, never kStale
  kPtpSynced,  // deadline semantics apply across hosts
  kNtpSynced,  // deadline semantics apply across hosts
};

// R3 support matrix: the portable contracts a backend can natively honor.
// Divergence over emulation — where the answer is "no", the library never
// silently emulates; the application decides at wiring time (M6-A6).
enum class Contract : std::uint8_t {
  kLatestOnly,        // LatestMailbox depth-1 semantics
  kBoundedQueue,      // queue<N> history
  kReliableQueue,     // reliable back-pressure on queue overflow
  kZeroCopyLoan,      // native loan path (no copy+serialize fallback)
  kLateJoinWarmStart, // M2: late subscriber reads the current slot value
  kRequestResponse,   // Client/Server carried natively
  kDeadline,          // measured (non-advisory) staleness semantics (R8)
  kSharedOwnership,   // D15 kShared last-writer-wins resolution
};

// Backend-specific configuration structs (D12): the portable API never
// grows a union of engine options. Every factory takes an isolation key
// (D17) — `name`, defaulting to a key derived from user + configured name;
// topics, segments, and introspection are namespaced by it, so two stacks
// on one host share nothing implicitly.

// In-process reach: built in, no dependencies, the reference semantics.
struct InProcessConfig {
  std::string name{};  // D17 isolation key (empty -> derived default)
};

// POSIX shm fallback backend (design.md): kernel-native primitives only,
// honestly partial via the support matrix. Same-host: clocks are shared by
// construction, so there is no ClockDomain to declare (R8).
struct PosixShmConfig {
  std::string name{};  // D17 isolation key
};

// iceoryx2 inter-process backend. Same-host: no ClockDomain needed (R8).
struct Iceoryx2Config {
  std::string name{};          // D17 isolation key
  std::string service_name{};  // backend-native service grouping
};

// Zenoh inter-host backend. The only reach that crosses a host boundary,
// hence the only config carrying the R8 clock declaration.
struct ZenohConfig {
  std::string name{};     // D17 isolation key
  std::string locator{};  // e.g. "tcp/robot-nav:7447"
  ClockDomain clock = ClockDomain::kUnsynced;  // R8/D12
};

// The messaging session. Move-only; endpoints must not outlive the Domain
// that minted them (application-owned composition, ADR 0005).
class Domain {
 public:
  // Per-backend factories (D12). Construction never throws; a factory for
  // a backend not compiled into this build yields a Domain whose endpoint
  // handles carry kUnsupportedReach (M8-A2) — never a silent in-process
  // fallback.
  static Domain InProcess(InProcessConfig config = {});
  static Domain PosixShm(PosixShmConfig config = {});
  static Domain Iceoryx2(Iceoryx2Config config = {});
  static Domain Zenoh(ZenohConfig config = {});

  Domain(Domain&& other) noexcept;
  Domain& operator=(Domain&& other) noexcept;
  Domain(const Domain&) = delete;
  Domain& operator=(const Domain&) = delete;
  ~Domain();

  // Create the write end of a topic. Never throws; check the handle's
  // status() (D18). Order-independent: Advertise after Subscribe is normal
  // (M14-A1). Transport memory is sized here from the declared QoS (R7).
  template <typename T>
  Publisher<T> Advertise(std::string_view topic, const Qos& qos = {});

  // Create the read end of a topic. Never throws; check status() (D18).
  // Subscribe before the publisher exists is normal, not an error —
  // freshness is simply kNone until matching happens (M14-A1).
  template <typename T>
  Subscriber<T> Subscribe(std::string_view topic, const Qos& qos = {});

  // Create the serve end of a typed RPC topic (D10: no hidden threads —
  // the application polls the returned endpoint from a loop it owns).
  template <typename Req, typename Rsp>
  Server<Req, Rsp> Serve(std::string_view topic);

  // Create the call end of a typed RPC topic (D11).
  template <typename Req, typename Rsp>
  ::xmotion::messaging::Client<Req, Rsp> Client(std::string_view topic);

  // D12/M6-A6: query the R3 per-reach support matrix at wiring time — the
  // same table design.md publishes, as a runtime fact, so divergences are
  // wiring-time knowledge, never field surprises.
  bool Supports(Contract contract) const noexcept;

  // D16: the one bounded readiness barrier — kMatched exactly when every
  // listed endpoint has >= 1 peer, kDeadlineExpired otherwise (M14-A2).
  // Enough for any launcher to sequence a stack before releasing motion;
  // there is no lifecycle framework and no match-event callback stream.
  WaitStatus WaitUntilMatched(std::initializer_list<const Endpoint*> endpoints,
                              Duration deadline) const;

  // Human-readable reach identifier ("in-process", "iceoryx2", ...), for
  // logs and stated-divergence messages (M6).
  std::string_view reach_name() const noexcept;

 private:
  Domain() noexcept = default;

  detail::DomainImpl* impl_ = nullptr;
};

}  // namespace messaging
}  // namespace xmotion

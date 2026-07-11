/*
 * domain.cpp
 *
 * Non-template definitions of the portable API: Domain lifecycle and
 * factories (D12), the D16 readiness barrier, the Endpoint base, and the
 * process-global DomainState registry (D17 isolation keys).
 *
 * The library owns no threads (R3): everything here runs on caller threads;
 * WaitUntilMatched blocks only the CALLER, bounded by its deadline (see the
 * poll-vs-condvar decision recorded at its definition).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include "xmmsg/messaging.hpp"

#include <unistd.h>  // geteuid — D17 default key derivation (user + name)

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xmotion {
namespace messaging {

namespace detail {

namespace {

std::mutex g_registry_mutex;

std::map<std::string, std::weak_ptr<DomainState>>& Registry() {
  static auto* registry = new std::map<std::string, std::weak_ptr<DomainState>>();
  return *registry;  // leaked intentionally: safe at any shutdown order
}

}  // namespace

std::string DeriveIsolationKey(const std::string& configured_name) {
  // D17 default, now SPECIFIED (wire-contract §6.2, resolved at P1b):
  //   key := "u" <euid decimal> "." <sanitized-domain-name>
  // where the sanitizer folds uppercase to lowercase and maps every byte
  // outside [a-z0-9_] to '_'. The numeric euid (not the login name) keeps
  // the derivation deterministic and passwd-free, and a foreign participant
  // reproduces it from getuid() alone. Backend resource names prefix this
  // key (POSIX shm: "/xmmsg.<key>.<topic>", shm_segment.hpp).
  const std::string base =
      configured_name.empty() ? "default" : configured_name;
  std::string sanitized;
  sanitized.reserve(base.size());
  for (char c : base) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
      sanitized += c;
    } else if (c >= 'A' && c <= 'Z') {
      sanitized += static_cast<char>(c - 'A' + 'a');
    } else {
      sanitized += '_';
    }
  }
  return "u" + std::to_string(static_cast<unsigned long>(::geteuid())) + "." +
         sanitized;
}

std::shared_ptr<DomainState> AcquireDomainState(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  auto& weak = Registry()[key];
  auto state = weak.lock();
  if (state == nullptr) {
    state = std::make_shared<DomainState>(key);
    weak = state;
  }
  return state;  // dies with the last Domain holding this key
}

}  // namespace detail

// ---- Endpoint base ---------------------------------------------------------

Endpoint::Endpoint() noexcept = default;

Endpoint::~Endpoint() {
  delete impl_;  // virtual: typed impls unregister from their topic (D7)
}

Endpoint::Endpoint(Endpoint&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

Endpoint& Endpoint::operator=(Endpoint&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

std::size_t Endpoint::MatchedCount() const noexcept {
  return impl_ != nullptr ? impl_->MatchedCount() : 0;
}

void* Endpoint::Native() noexcept {
  // In-process reach: no engine underneath (R3 escape-hatch contract).
  return nullptr;
}

// ---- Domain lifecycle ------------------------------------------------------

Domain Domain::InProcess(InProcessConfig config) {
  Domain domain;
  domain.impl_ = new detail::DomainImpl(
      detail::DomainImpl::Reach::kInProcess,
      detail::AcquireDomainState(detail::DeriveIsolationKey(config.name)));
  return domain;
}

// P1b: the POSIX shm fallback backend. When compiled in (the default —
// it has no dependencies to creep, design.md "Backend seam") the Domain
// carries only its isolation key: the cross-process "registry" is /dev/shm
// itself, one named segment per topic (detail/shm_segment.hpp). When
// compiled out, endpoints carry kUnsupportedReach (M8-A2) — never a silent
// in-process fallback.
Domain Domain::PosixShm(PosixShmConfig config) {
  Domain domain;
#if defined(XMMESSAGING_HAS_POSIX_SHM)
  domain.impl_ = new detail::DomainImpl(
      detail::DomainImpl::Reach::kPosixShm, nullptr,
      detail::DeriveIsolationKey(config.name));
#else
  (void)config;
  domain.impl_ =
      new detail::DomainImpl(detail::DomainImpl::Reach::kPosixShm, nullptr);
#endif
  return domain;
}

// The two backend factories below yield Domains whose endpoints carry
// kUnsupportedReach (M8-A2): their backends are not compiled into this
// build tier yet (iceoryx2: P1; Zenoh: P2).

Domain Domain::Iceoryx2(Iceoryx2Config config) {
  (void)config;
  Domain domain;
  domain.impl_ =
      new detail::DomainImpl(detail::DomainImpl::Reach::kIceoryx2, nullptr);
  return domain;
}

Domain Domain::Zenoh(ZenohConfig config) {
  (void)config;
  Domain domain;
  domain.impl_ =
      new detail::DomainImpl(detail::DomainImpl::Reach::kZenoh, nullptr);
  return domain;
}

Domain::Domain(Domain&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

Domain& Domain::operator=(Domain&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

Domain::~Domain() { delete impl_; }

bool Domain::Supports(Contract contract) const noexcept {
  if (impl_ == nullptr || !impl_->available()) {
    // Unavailable-backend domains support nothing (M8-A2/M6-A6).
    return false;
  }
  if (impl_->reach_ == detail::DomainImpl::Reach::kPosixShm) {
    // The honestly-partial P1b matrix (design.md; rationale per contract in
    // detail/posix_shm.hpp). M6-A6 asserts this table verbatim.
    switch (contract) {
      case Contract::kLatestOnly:
      case Contract::kBoundedQueue:
      case Contract::kLateJoinWarmStart:
      case Contract::kDeadline:
        return true;
      case Contract::kReliableQueue:
      case Contract::kZeroCopyLoan:
      case Contract::kRequestResponse:
      case Contract::kSharedOwnership:
        return false;
    }
    return false;
  }
  // The in-process reach is the reference semantics: every portable
  // contract is honored natively (design.md support matrix, first column).
  (void)contract;
  return true;
}

WaitStatus Domain::WaitUntilMatched(
    std::initializer_list<const Endpoint*> endpoints, Duration deadline) const {
  if (impl_ == nullptr || !impl_->available()) {
    // Nothing can ever match on an unavailable reach; the barrier answer
    // is immediate and honest rather than a blind sleep.
    return endpoints.size() == 0 ? WaitStatus::kMatched
                                 : WaitStatus::kDeadlineExpired;
  }
  const auto all_matched = [&endpoints] {
    for (const Endpoint* endpoint : endpoints) {
      if (endpoint != nullptr && endpoint->MatchedCount() == 0) {
        return false;
      }
    }
    return true;
  };
  // Bounded monotonic poll (1 ms granularity), NOT a timed condvar wait —
  // a deliberate P0b decision, recorded here:
  //  - The deadline must be judged on the monotonic clock (R8). libstdc++
  //    implements monotonic condvar waits via pthread_cond_clockwait, which
  //    the GCC 11 libtsan (the Ubuntu 22.04 baseline, R1) does NOT
  //    intercept — every timed monotonic condvar wait produces false
  //    "double lock"/race reports, and TSan-cleanliness is a P0b gate.
  //  - This is a wiring-time launcher verb called once per startup; 1 ms
  //    match-detection latency is irrelevant, and the poll reads only the
  //    lock-free matching counters (no interaction with wiring locks).
  // TODO(P1): move the park onto the detail::CondvarWaiter seam (event-
  // driven wake on endpoint registration) once the baseline toolchain's
  // TSan intercepts pthread_cond_clockwait (GCC >= 12).
  const Timestamp deadline_at = ::xmotion::Now() + deadline;
  for (;;) {
    if (all_matched()) {
      return WaitStatus::kMatched;
    }
    const Duration remaining = deadline_at - ::xmotion::Now();
    if (remaining <= Duration::zero()) {
      return WaitStatus::kDeadlineExpired;
    }
    std::this_thread::sleep_for(
        remaining < std::chrono::milliseconds(1)
            ? remaining
            : Duration(std::chrono::milliseconds(1)));
  }
}

std::string_view Domain::reach_name() const noexcept {
  if (impl_ == nullptr) {
    return "moved-from";
  }
  switch (impl_->reach_) {
    case detail::DomainImpl::Reach::kInProcess:
      return "in-process";
    case detail::DomainImpl::Reach::kPosixShm:
      return "posix-shm";
    case detail::DomainImpl::Reach::kIceoryx2:
      return "iceoryx2";
    case detail::DomainImpl::Reach::kZenoh:
      return "zenoh";
  }
  return "unknown";
}

}  // namespace messaging
}  // namespace xmotion

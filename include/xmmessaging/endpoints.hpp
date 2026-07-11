/*
 * endpoints.hpp
 *
 * The endpoint handles: Publisher<T>, Subscriber<T>, Server<Req,Rsp>,
 * Client<Req,Rsp>, and the Loan/Request/Result value types they exchange.
 * All handles are minted by a Domain (domain.hpp) at wiring time and used
 * on the hot path — never name-lookup inside a loop. Construction never
 * throws; every handle carries a queryable status (D18). Teardown is scope
 * exit — RAII, no unsubscribe ceremony (D7). P0a: declarations only.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "xmmessaging/payload_traits.hpp"
#include "xmmessaging/qos.hpp"
#include "xmmessaging/sample.hpp"
#include "xmmessaging/status.hpp"

namespace xmotion {
namespace messaging {

class Domain;

namespace detail {
class EndpointImpl;
struct EndpointAccess;  // P0b: introspect/test seam over the impl pointer
template <typename T>
class LoanPool;  // P0b: publisher-side loan cell pool (detail/in_process.hpp)
template <typename Req, typename Rsp>
class RpcTopic;  // P0b part 2: RPC slot machinery (detail/in_process.hpp)
}  // namespace detail

// Common endpoint surface: the D16 readiness knobs and the R3 escape hatch.
// Non-polymorphic base; endpoints are never owned or deleted through it —
// it exists so heterogeneous endpoint sets can be enumerated (D16).
class Endpoint {
 public:
  // D16: number of currently matched peers; tracks join/leave through
  // peer death and rejoin (M14-A5). The readiness surface is this plus
  // Domain::WaitUntilMatched — no match-event callbacks (hidden execution,
  // R3).
  std::size_t MatchedCount() const noexcept;

  // R3 native escape hatch: the underlying backend handle, explicitly
  // non-portable by declaration. Type and lifetime are the backend's;
  // nullptr on the in-process reach (no engine underneath).
  void* Native() noexcept;

 protected:
  friend struct detail::EndpointAccess;  // P0b: introspect (D9) reads impl_

  Endpoint() noexcept;
  ~Endpoint();
  Endpoint(Endpoint&& other) noexcept;
  Endpoint& operator=(Endpoint&& other) noexcept;
  Endpoint(const Endpoint&) = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  detail::EndpointImpl* impl_ = nullptr;
};

// A writable slot in transport memory (QoS knob "Loan"): construct the
// payload in place, then Publish() to hand ownership back — zero-copy on
// the reaches that honor it natively, copy+serialize fallback on the
// network reach (design.md QoS vocabulary). Move-only.
template <typename T>
class Loan {
 public:
  // M6-A4: the zero-copy path is a compile-time fact at the wiring site.
  // The static_assert lives in Publisher<T>::Loan() — the only place a
  // Loan is minted — NOT at class scope: overload resolution of
  // Publisher::Publish(...) must be able to complete this type for any
  // payload without tripping the zero-copy requirement (P0b fix; a
  // class-scope assert made Publish(braced-init) ill-formed for every
  // non-trivially-copyable payload).

  Loan(Loan&& other) noexcept;
  Loan& operator=(Loan&& other) noexcept;
  Loan(const Loan&) = delete;
  Loan& operator=(const Loan&) = delete;
  // An unpublished loan is returned to the pool at scope exit (RAII).
  ~Loan();

  // D18 pattern: Loan() never throws; exhaustion is queryable here (the
  // "loan-exhausted" failure mode of design.md, kept off PublishStatus so
  // D8's publish accounting stays two-valued).
  LoanStatus status() const noexcept { return status_; }

  // Accessing an exhausted loan is a contract violation (debug-assert,
  // mirrors the D2 kNone-dereference rule).
  T& operator*() noexcept {
    assert(status_ == LoanStatus::kOk);
    return *slot_;
  }
  T* operator->() noexcept {
    assert(status_ == LoanStatus::kOk);
    return slot_;
  }

 private:
  template <typename>
  friend class Publisher;  // loans are minted by Publisher<T>::Loan() only

  Loan() noexcept = default;

  T* slot_ = nullptr;
  LoanStatus status_ = LoanStatus::kExhausted;
  // P0b: owning pool, so an unpublished loan can return its cell at scope
  // exit without a lookup (RAII contract above).
  detail::LoanPool<T>* pool_ = nullptr;
};

// The write end of a topic. Minted by Domain::Advertise (D18: check
// status() — construction never throws).
template <typename T>
class Publisher : public Endpoint {
 public:
  static_assert(is_payload_v<T>,
                "xmMessaging: a payload must be a plain movable object type "
                "(design.md payload requirements)");

  Publisher(Publisher&& other) noexcept;
  Publisher& operator=(Publisher&& other) noexcept;
  ~Publisher();

  // D18: kOk, kOwnershipRefused (D15), kTypeMismatch (R6), or
  // kUnsupportedReach (M8-A2). Publishing on a non-kOk handle is a
  // contract violation (debug-assert).
  AdvertiseStatus status() const noexcept;

  // Zero-copy publication (QoS knob "Loan"): loan -> construct in place ->
  // Publish. Never blocks, never throws; exhaustion is on the handle.
  ::xmotion::messaging::Loan<T> Loan();

  // Publish by copy. D8: never blocks internally; kWouldBlock only on
  // reliable+bounded-full (nothing enqueued); best-effort always kOk. On
  // latest-only it never fails for capacity — the slot is overwritten and
  // the overwrite is counted (LatestMailbox contract, M1-A1).
  PublishStatus Publish(const T& value);

  // Publish a loaned value (first hop: origin stamp = publish stamp,
  // hop count = 0 — M13-A2). D13: snapshots the calling thread's active
  // telemetry context into the envelope (null context if none).
  PublishStatus Publish(::xmotion::messaging::Loan<T>&& loan);

  // D14: publish a value derived from consumed upstream samples — origin
  // stamp is preserved from the OLDEST consumed input (information is only
  // as fresh as its stalest ingredient), hop count increments.
  template <typename... Upstream>
  PublishStatus PublishDerived(::xmotion::messaging::Loan<T>&& loan,
                               const Sample<Upstream>&... upstream);

 private:
  friend class Domain;  // publishers are minted by Domain::Advertise only
  Publisher() noexcept = default;
};

// The read end of a topic. Each subscriber owns an independent mailbox or
// queue (D7): one subscriber's take never disturbs another's (M2-A3).
template <typename T>
class Subscriber : public Endpoint {
 public:
  static_assert(is_payload_v<T>,
                "xmMessaging: a payload must be a plain movable object type "
                "(design.md payload requirements)");

  Subscriber(Subscriber&& other) noexcept;
  Subscriber& operator=(Subscriber&& other) noexcept;
  ~Subscriber();

  // D18: kOk, kTypeMismatch (R6), or kUnsupportedReach (M8-A2).
  SubscribeStatus status() const noexcept;

  // D1: the latest-only take verb — wait-free, allocation-free (R7).
  // Always returns a Sample; emptiness/staleness are freshness states, not
  // return types. A late joiner's first take warm-starts from the current
  // slot value with its ORIGINAL stamp (D6).
  Sample<T> TakeLatest();

  // Queue take verb: pops the oldest queued value, or a kNone sample when
  // the queue is empty. Never blocks (D5: the loop owns its cadence — no
  // blocking wait verb on the paced-consumer surface).
  Sample<T> TryTake();

 private:
  friend class Domain;  // subscribers are minted by Domain::Subscribe only
  Subscriber() noexcept = default;
};

// The request as taken by a server (D10): reuses the D1/D2 Sample surface —
// an empty inbox is kNone, not a different type — and carries the reply
// correlation token plus the caller's telemetry context.
template <typename Req>
class Request {
 public:
  // D2 surface: kNone when the inbox is empty; kFresh otherwise (a request
  // has no wiring-time deadline on the server side).
  Freshness freshness() const noexcept { return freshness_; }

  // Age of the request since the client stamped it (same-host semantics,
  // R8).
  Duration age() const noexcept { return ::xmotion::Now() - stamp_; }

  // D10: the caller's telemetry context — replying under a ContextGuard of
  // this links the server's handler span into the client's trace (M5-A1).
  ::xmotion::telemetry::Context context() const noexcept { return context_; }

  // D2 rule: dereferencing a kNone request is a contract violation.
  const Req& operator*() const noexcept {
    assert(freshness_ != Freshness::kNone);
    return value_;
  }
  const Req* operator->() const noexcept {
    assert(freshness_ != Freshness::kNone);
    return &value_;
  }

 private:
  template <typename, typename>
  friend class Server;  // requests are minted by TakeRequest only
  template <typename, typename>
  friend class detail::RpcTopic;  // P0b: the machinery behind TakeRequest

  Req value_{};
  ::xmotion::telemetry::Context context_{};
  Timestamp stamp_{};
  std::uint64_t correlation_ = 0;  // reply/late-discard token (M5-A2)
  Freshness freshness_ = Freshness::kNone;
};

// The serve end of a typed RPC topic (D10): the server owns NO threads
// (R3) — it is a take/reply endpoint polled from an application-owned
// loop, like every other loop in the family.
template <typename Req, typename Rsp>
class Server : public Endpoint {
 public:
  static_assert(is_payload_v<Req> && is_payload_v<Rsp>,
                "xmMessaging: request and response must be plain movable "
                "object types (design.md payload requirements)");

  Server(Server&& other) noexcept;
  Server& operator=(Server&& other) noexcept;
  ~Server();

  // D18: kOk, kOwnershipRefused (one server per topic), kTypeMismatch,
  // or kUnsupportedReach.
  AdvertiseStatus status() const noexcept;

  // D10: never blocks; an empty inbox is a kNone request.
  Request<Req> TakeRequest();

  // Reply to a taken request, correlated by its token. kExpired when the
  // caller's deadline already passed — the reply is discarded, never
  // surfacing on a later call (M5-A2).
  ReplyStatus Reply(const Request<Req>& request, const Rsp& response);

  // D10: the bounded-park verb — a server loop neither busy-spins nor
  // sleeps blind. Returns when work arrives, shutdown begins, or max_park
  // elapses, whichever is first; true iff work is pending.
  bool WaitForWorkOrShutdown(Duration max_park);

 private:
  friend class Domain;  // servers are minted by Domain::Serve only
  Server() noexcept = default;
};

// The call result (D11): status + value, mirroring Sample<T>; the value is
// only accessible on kOk.
template <typename Rsp>
class Result {
 public:
  // D11: kOk, kDeadlineExpired, kNoServer — distinct by contract (M5).
  CallStatus status() const noexcept { return status_; }

  // Dereference on a non-kOk result is a contract violation (D2 rule).
  const Rsp& operator*() const noexcept {
    assert(status_ == CallStatus::kOk);
    return value_;
  }
  const Rsp* operator->() const noexcept {
    assert(status_ == CallStatus::kOk);
    return &value_;
  }

 private:
  template <typename, typename>
  friend class Client;  // results are minted by Call only
  template <typename, typename>
  friend class detail::RpcTopic;  // P0b: the machinery behind Call

  Rsp value_{};
  CallStatus status_ = CallStatus::kNoServer;
};

// The call end of a typed RPC topic (D11).
template <typename Req, typename Rsp>
class Client : public Endpoint {
 public:
  static_assert(is_payload_v<Req> && is_payload_v<Rsp>,
                "xmMessaging: request and response must be plain movable "
                "object types (design.md payload requirements)");

  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;
  ~Client();

  // D18: kOk, kTypeMismatch, or kUnsupportedReach.
  SubscribeStatus status() const noexcept;

  // D11: the deadline is mandatory — there is deliberately NO deadline-less
  // overload; an unbounded wait is unrepresentable (M5-A4). Blocks at most
  // until the deadline. kDeadlineExpired and kNoServer are distinct; a
  // late reply is discarded by correlation (M5-A2/A3). The request rides
  // with the calling thread's telemetry context (D13/M5-A1).
  Result<Rsp> Call(const Req& request, Duration deadline);

 private:
  friend class Domain;  // clients are minted by Domain::Client only
  Client() noexcept = default;
};

// D9: endpoint counters are programmatically queryable, in addition to the
// messaging.* telemetry instruments and the R5 CLI — scenarios and
// applications reconcile conservation exactly, in code (M3).
namespace introspect {

// Best-effort overflow drops at this subscriber's queue (M3-A2:
// delivered + drops == published, exactly).
template <typename T>
std::uint64_t DropCount(const Subscriber<T>& subscriber);

// Latest-only values overwritten before this subscriber read them (M1-A1).
template <typename T>
std::uint64_t OverwriteCount(const Subscriber<T>& subscriber);

// Values taken by this subscriber.
template <typename T>
std::uint64_t TakeCount(const Subscriber<T>& subscriber);

// Values accepted from this publisher.
template <typename T>
std::uint64_t PublishCount(const Publisher<T>& publisher);

// Reliable publishes refused with kWouldBlock (M3-A1).
template <typename T>
std::uint64_t RefusedCount(const Publisher<T>& publisher);

}  // namespace introspect

}  // namespace messaging
}  // namespace xmotion

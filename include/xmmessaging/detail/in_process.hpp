/*
 * detail/in_process.hpp
 *
 * The in-process reach — the reference semantics of every portable
 * contract (design.md "One API, three reaches"). This header holds the
 * typed machinery (topics, endpoint impls, fan-out) plus the definitions
 * of every template member the portable API declares. It is included at
 * the END of domain.hpp and must not be included directly.
 *
 * Architecture (P0b):
 *
 *   Domain --*--> DomainState (process-global registry, keyed by the D17
 *                 isolation key: two Domains with one key share topics,
 *                 different keys share NOTHING — M14-A4)
 *   DomainState --*--> Topic<T> (keyed by topic string; carries the R6
 *                 schema hash and the D15 ownership state)
 *   Topic<T> --> master LatestSlot   (warm-start source for late joiners,
 *                 D6: the slot exists, a new subscriber reads it — with
 *                 the ORIGINAL envelope, so age never lies)
 *            --*--> SubCore<T>       (one per subscriber: its OWN mailbox
 *                 or ring + its OWN counters, D7/D8)
 *
 * Fan-out semantics (D8, M3), stated precisely:
 *   - Publish delivers to EVERY registered subscriber structure.
 *   - reliable + queue<N>: before anything is enqueued anywhere, every
 *     queue-shaped subscriber ring is checked; if ANY is full the publish
 *     is refused with kWouldBlock and NOTHING is enqueued (all-or-nothing,
 *     so per-subscriber conservation stays exact: every accepted publish
 *     reached every ring). The refusal counts on the publisher. Safe
 *     against racing consumers: rings are SPSC, only the consumer can
 *     make one LESS full between the check and the push.
 *   - best-effort + queue<N>: Publish always returns kOk; a full ring
 *     drops the INCOMING value for that subscriber only (drop-newest) and
 *     counts the drop on that subscriber (delivered + drops == published,
 *     exactly — M3-A2).
 *   - latest-only subscribers never refuse and never drop: the slot is
 *     overwritten (LatestMailbox guarantee 1); kWouldBlock can never
 *     originate from a latest-only mailbox (M1-A1).
 *
 * Overwrite accounting (LatestMailbox guarantee 3): counted per subscriber
 * by ORDINAL-GAP accounting at take time — when a take observes ordinal k
 * and this subscriber last consumed ordinal j, the k-j-1 values in between
 * were overwritten before this subscriber read them. This is exact at
 * every quiescent point (M1-A1/A5 reconcile it exactly). The writer
 * deliberately does NOT count per-reader overwrites: with lock-free
 * readers, a writer-side "was it read?" check races the reader's own take
 * and can double-count — reader-side gap accounting cannot. The counter
 * is materialized by takes; a subscriber that stops taking stops
 * materializing (its unread backlog shows up on its next take or at drain).
 *
 * Hot-path discipline (R7): after wiring, Publish/TakeLatest/TryTake
 * allocate nothing and take no locks on the exclusive-ownership path.
 * Wiring-time paths (Advertise/Subscribe/destruction/WaitUntilMatched)
 * lock DomainState::mutex_. Declared-shared publishers serialize on the
 * topic writer mutex (see TopicBase::shared_write_mutex_).
 *
 * Threads: none. The library never spawns anything (R3).
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/handles.hpp"
#include "xmmessaging/detail/bounded_queue.hpp"
#include "xmmessaging/detail/envelope.hpp"
#include "xmmessaging/detail/latest_slot.hpp"
#include "xmmessaging/detail/mail_record.hpp"
#include "xmmessaging/detail/schema_hash.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// The latest-only mailbox type for a payload: the wait-free seqlock slot
// for trivially copyable payloads, the documented mutex fallback otherwise
// (see latest_slot.hpp).
template <typename T>
using LatestSlotFor =
    std::conditional_t<std::is_trivially_copyable_v<MailRecord<T>>,
                       LatestSlot<T, HeapPlacement, CondvarWaiter>,
                       MutexLatestSlot<T>>;

// Lineage info for PublishDerived (D14).
struct DerivedInfo {
  std::int64_t origin_ns = 0;  // oldest consumed input's origin stamp
  std::uint32_t hops = 0;      // max upstream hops + 1, saturated
};

// ---------------------------------------------------------------------------
// Loan pool: fixed publisher-side scratch cells for the Loan verb, allocated
// at wiring time. Acquire/Release are lock-free (one CAS loop over a bitmask)
// and allocation-free (R7). Loan payloads are trivially copyable by
// static_assert (endpoints.hpp), hence trivially destructible.
// ---------------------------------------------------------------------------
template <typename T>
class LoanPool {
 public:
  static constexpr std::uint32_t kSlots = 8;

  T* Acquire() noexcept {
    std::uint32_t mask = in_use_.load(std::memory_order_relaxed);
    for (;;) {
      std::uint32_t free_index = kSlots;
      for (std::uint32_t i = 0; i < kSlots; ++i) {
        if ((mask & (1u << i)) == 0u) {
          free_index = i;
          break;
        }
      }
      if (free_index == kSlots) {
        return nullptr;  // exhausted — surfaces as LoanStatus::kExhausted
      }
      if (in_use_.compare_exchange_weak(mask, mask | (1u << free_index),
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        T* slot = SlotAt(free_index);
        if constexpr (std::is_default_constructible_v<T>) {
          ::new (static_cast<void*>(slot)) T();
        }
        return slot;
      }
    }
  }

  void Release(T* slot) noexcept {
    const auto index = static_cast<std::uint32_t>(
        (reinterpret_cast<unsigned char*>(slot) - storage_) / sizeof(T));
    assert(index < kSlots && "Loan released to a pool it was not minted by");
    in_use_.fetch_and(~(1u << index), std::memory_order_acq_rel);
  }

 private:
  T* SlotAt(std::uint32_t index) noexcept {
    return reinterpret_cast<T*>(storage_ + index * sizeof(T));
  }

  std::atomic<std::uint32_t> in_use_{0};
  alignas(alignof(T)) unsigned char storage_[kSlots * sizeof(T)] = {};
};

// ---------------------------------------------------------------------------
// Per-subscriber state: an independent mailbox or ring (D7) plus the D9
// always-on counters and the R11 standard-schema instruments. Instrument
// name strings are members because the telemetry SDK interns names by
// pointer — they must outlive every hot-path handle use.
// ---------------------------------------------------------------------------
template <typename T>
struct SubCore {
  SubCore(const Qos& qos, const std::string& topic)
      : history(qos.history),
        deadline(qos.deadline),
        n_take("messaging.sub." + topic + ".take_count"),
        n_drop("messaging.sub." + topic + ".drop_count"),
        n_overwrite("messaging.sub." + topic + ".overwrite_count"),
        n_deadline_miss("messaging.sub." + topic + ".deadline_miss_count"),
        n_take_age("messaging.sub." + topic + ".take_age_us"),
        n_queue_depth("messaging.sub." + topic + ".queue_depth"),
        t_take(::xmotion::telemetry::GetCounter(n_take)),
        t_drop(::xmotion::telemetry::GetCounter(n_drop)),
        t_overwrite(::xmotion::telemetry::GetCounter(n_overwrite)),
        t_deadline_miss(::xmotion::telemetry::GetCounter(n_deadline_miss)),
        t_take_age(::xmotion::telemetry::GetHistogram(n_take_age)),
        t_queue_depth(::xmotion::telemetry::GetGauge(n_queue_depth)) {
    if (history.kind() == History::Kind::kLatestOnly) {
      latest = std::make_unique<LatestSlotFor<T>>();
    } else {
      queue = std::make_unique<BoundedQueue<T, HeapPlacement>>(history.depth());
    }
  }

  const History history;
  const std::optional<Duration> deadline;

  // R11 §7 instrument names (stable storage) + handles.
  const std::string n_take, n_drop, n_overwrite, n_deadline_miss, n_take_age,
      n_queue_depth;
  ::xmotion::telemetry::Counter t_take, t_drop, t_overwrite, t_deadline_miss;
  ::xmotion::telemetry::Histogram t_take_age;
  ::xmotion::telemetry::Gauge t_queue_depth;

  // Exactly one of these is engaged, by declared History (D7).
  std::unique_ptr<LatestSlotFor<T>> latest;
  std::unique_ptr<BoundedQueue<T, HeapPlacement>> queue;

  // D9 always-on counters (exact, independent of any telemetry binding).
  std::atomic<std::uint64_t> take_count{0};
  std::atomic<std::uint64_t> drop_count{0};
  std::atomic<std::uint64_t> overwrite_count{0};
  std::atomic<std::uint64_t> deadline_miss_count{0};

  // Ordinal-gap accounting state (see the header comment). Owned by the
  // consuming thread; atomic so introspection from other threads is clean.
  std::atomic<std::uint64_t> last_consumed_ordinal{0};
  // D3: the deadline-miss event fires on the Fresh->Stale TRANSITION.
  std::atomic<bool> stale_latched{false};
};

// ---------------------------------------------------------------------------
// Per-publisher state.
// ---------------------------------------------------------------------------
template <typename T>
struct PubCore {
  PubCore(const Qos& qos, const std::string& topic)
      : reliability(qos.reliability),
        shared_ownership(qos.ownership == Ownership::kShared),
        n_publish("messaging.pub." + topic + ".publish_count"),
        n_refused("messaging.pub." + topic + ".refused_count"),
        n_bytes("messaging.pub." + topic + ".bytes"),
        t_publish(::xmotion::telemetry::GetCounter(n_publish)),
        t_refused(::xmotion::telemetry::GetCounter(n_refused)),
        t_bytes(::xmotion::telemetry::GetCounter(n_bytes)) {}

  const Reliability reliability;
  // Declared-shared publishers serialize on the topic writer mutex; the
  // exclusive default never touches it (see TopicBase).
  const bool shared_ownership;

  const std::string n_publish, n_refused, n_bytes;
  ::xmotion::telemetry::Counter t_publish, t_refused, t_bytes;

  // D9 always-on counters.
  std::atomic<std::uint64_t> publish_count{0};
  std::atomic<std::uint64_t> refused_count{0};
  std::atomic<std::uint64_t> bytes{0};

  LoanPool<T> pool;
};

// ---------------------------------------------------------------------------
// Topic registry entry. TopicBase is the type-erased face (registry,
// matching counts, ownership); Topic<T> adds the typed fan-out machinery.
// ---------------------------------------------------------------------------
class TopicBase {
 public:
  TopicBase(std::string name, std::uint64_t schema_hash,
            const std::type_info& type)
      : name_(std::move(name)), schema_hash_(schema_hash), type_(&type) {}
  virtual ~TopicBase() = default;

  const std::string name_;
  // R6 gate. INTERIM hash input at P0b (schema_hash.hpp); the type_info
  // pointer additionally guards the static_cast to Topic<T> — in-process,
  // typeid identity is exact, so a hash collision can never reinterpret.
  const std::uint64_t schema_hash_;
  const std::type_info* const type_;

  // D16 matching counts (read lock-free by MatchedCount()).
  std::atomic<std::size_t> publisher_count_{0};
  std::atomic<std::size_t> subscriber_count_{0};

  // D15 ownership state — guarded by DomainState::mutex_ (wiring only).
  Ownership declared_ownership_ = Ownership::kExclusive;

  // 1-based ordinal of accepted publishes (see mail_record.hpp).
  std::atomic<std::uint64_t> accepted_ordinal_{0};

  // Writer-writer serialization for DECLARED-SHARED topics only (D15
  // last-writer-wins: the publish stamp is taken inside this lock, so lock
  // order == stamp order and the resolution is deterministic).
  // TODO(P1): lock-free multi-writer (ordinal-CAS seqlock variant) if a
  // shared-ownership topic ever lands on a hot path.
  std::mutex shared_write_mutex_;
};

template <typename T>
class Topic final : public TopicBase {
 public:
  // Fixed fan-out table so the publish path can iterate lock-free (R7:
  // bounded resources — a hard per-topic subscriber bound is a wiring-time
  // fact, not a hidden growth path).
  static constexpr std::size_t kMaxSubscribers = 64;

  Topic(std::string name, std::uint64_t schema_hash)
      : TopicBase(std::move(name), schema_hash, typeid(T)) {
    for (auto& slot : sub_slots_) {
      slot.store(nullptr, std::memory_order_relaxed);
    }
  }

  // ---- wiring side (DomainState::mutex_ held by the caller) --------------

  AdvertiseStatus AddPublisher(const Qos& qos,
                               std::shared_ptr<PubCore<T>>* out) {
    if (publisher_count_.load(std::memory_order_relaxed) > 0 &&
        (declared_ownership_ == Ownership::kExclusive ||
         qos.ownership == Ownership::kExclusive)) {
      // D15: shared ownership is a declaration made by BOTH publishers.
      return AdvertiseStatus::kOwnershipRefused;
    }
    declared_ownership_ = qos.ownership;
    *out = std::make_shared<PubCore<T>>(qos, name_);
    publisher_count_.fetch_add(1, std::memory_order_relaxed);
    return AdvertiseStatus::kOk;
  }

  void RemovePublisher(PubCore<T>* /*core*/) {
    publisher_count_.fetch_sub(1, std::memory_order_relaxed);
    // When the last publisher leaves, the next Advertise re-establishes
    // ownership from scratch (M14-A5: kill + re-advertise succeeds).
  }

  std::shared_ptr<SubCore<T>> AddSubscriber(const Qos& qos) {
    auto core = std::make_shared<SubCore<T>>(qos, name_);
    if (core->latest != nullptr) {
      // D6 warm start: seed the new mailbox from the current slot value,
      // ORIGINAL envelope included (M2-A1: the stamp is the publish stamp,
      // so age reports the truth). Values published before the join are
      // not charged to this subscriber's overwrite counter.
      MailRecord<T> current;
      if (master_.Load(current)) {
        core->latest->Store(current);
        core->last_consumed_ordinal.store(current.ordinal - 1,
                                          std::memory_order_relaxed);
      } else {
        core->last_consumed_ordinal.store(
            accepted_ordinal_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
      }
    }
    for (auto& slot : sub_slots_) {
      if (slot.load(std::memory_order_relaxed) == nullptr) {
        // Publish order: the core is fully seeded BEFORE it becomes
        // visible to the fan-out loop (release), so a racing publish can
        // only ever add newer records on top of the seed.
        slot.store(core.get(), std::memory_order_release);
        sub_retained_.push_back(core);
        subscriber_count_.fetch_add(1, std::memory_order_relaxed);
        return core;
      }
    }
    // Open item (P0b): no distinct status exists for slot exhaustion; the
    // bound is documented and asserted. Raising kMaxSubscribers or adding
    // a status is a P1 API decision.
    assert(false && "xmMessaging: kMaxSubscribers exceeded on one topic");
    return core;  // unconnected: behaves as never-matched / kNone
  }

  void RemoveSubscriber(SubCore<T>* core) {
    for (auto& slot : sub_slots_) {
      if (slot.load(std::memory_order_relaxed) == core) {
        slot.store(nullptr, std::memory_order_release);
        subscriber_count_.fetch_sub(1, std::memory_order_relaxed);
        break;
      }
    }
    // The SubCore stays alive in sub_retained_ until the topic dies: a
    // publisher mid-fan-out may still hold the raw pointer it loaded
    // before the detach, and writing into a detached mailbox is harmless.
    // Memory is bounded by total joins per topic lifetime (P0b interim;
    // epoch-based reclamation is not warranted at this scale).
  }

  // ---- hot path -----------------------------------------------------------

  // derived == nullptr for a first-hop Publish (origin = stamp, hops = 0).
  PublishStatus PublishFrom(PubCore<T>& pub, const T& value,
                            const DerivedInfo* derived) {
    if (pub.shared_ownership) {
      std::lock_guard<std::mutex> lock(shared_write_mutex_);
      return Deliver(pub, value, derived);
    }
    return Deliver(pub, value, derived);
  }

  // Master slot read (warm start + late-join seeding).
  bool LoadMaster(MailRecord<T>& out) { return master_.Load(out); }

 private:
  PublishStatus Deliver(PubCore<T>& pub, const T& value,
                        const DerivedInfo* derived) {
    // Reliable pre-check: all-or-nothing (see the fan-out semantics in the
    // header comment). Latest-only mailboxes are never checked — they
    // cannot refuse (M1-A1).
    if (pub.reliability == Reliability::kReliable) {
      for (const auto& slot : sub_slots_) {
        const SubCore<T>* sub = slot.load(std::memory_order_acquire);
        if (sub != nullptr && sub->queue != nullptr && sub->queue->Full()) {
          pub.refused_count.fetch_add(1, std::memory_order_relaxed);
          pub.t_refused.Add();
          return PublishStatus::kWouldBlock;  // nothing was enqueued
        }
      }
    }

    MailRecord<T> record;
    const auto context_bytes =
        ::xmotion::telemetry::Inject(::xmotion::telemetry::CurrentContext());
    std::memcpy(record.envelope.context, context_bytes.data(),
                kEnvelopeContextSize);
    const std::int64_t now_ns = ::xmotion::Now().time_since_epoch().count();
    record.envelope.publish_stamp_ns = now_ns;  // inside the shared lock:
                                                // stamp order == lock order
    record.envelope.origin_stamp_ns =
        derived != nullptr ? derived->origin_ns : now_ns;  // D14
    record.envelope.hop_count = derived != nullptr ? derived->hops : 0;
    record.envelope.flags = 0;
    record.ordinal =
        accepted_ordinal_.fetch_add(1, std::memory_order_relaxed) + 1;
    record.payload = value;

    master_.Store(record);
    for (const auto& slot : sub_slots_) {
      SubCore<T>* sub = slot.load(std::memory_order_acquire);
      if (sub == nullptr) {
        continue;
      }
      if (sub->latest != nullptr) {
        sub->latest->Store(record);  // overwrite, never refuse
      } else if (sub->queue != nullptr) {
        if (sub->queue->TryPush(record)) {
          sub->t_queue_depth.Set(static_cast<double>(sub->queue->Size()));
        } else {
          // best-effort drop-newest: this subscriber's fact (D8).
          sub->drop_count.fetch_add(1, std::memory_order_relaxed);
          sub->t_drop.Add();
        }
      }
    }

    pub.publish_count.fetch_add(1, std::memory_order_relaxed);
    pub.bytes.fetch_add(sizeof(T), std::memory_order_relaxed);
    pub.t_publish.Add();
    pub.t_bytes.Add(static_cast<double>(sizeof(T)));
    return PublishStatus::kOk;
  }

  LatestSlotFor<T> master_;
  std::array<std::atomic<SubCore<T>*>, kMaxSubscribers> sub_slots_;
  std::vector<std::shared_ptr<SubCore<T>>> sub_retained_;  // wiring mutex
};

// ---------------------------------------------------------------------------
// Domain state: one per isolation key, shared by every Domain constructed
// with that key (D17). Owns the topic registry and the D16 match barrier.
// ---------------------------------------------------------------------------
struct DomainState {
  explicit DomainState(std::string key) : key_(std::move(key)) {}

  const std::string key_;
  std::mutex mutex_;  // wiring paths only — never publish/take
  // NOTE: WaitUntilMatched polls the lock-free matching counters instead of
  // parking on a condvar here — see the decision recorded in domain.cpp.
  std::map<std::string, std::shared_ptr<TopicBase>, std::less<>> topics_;

  // R11 per-domain gauges + wiring-side endpoint census.
  ::xmotion::telemetry::Gauge g_endpoints_{
      ::xmotion::telemetry::GetGauge("messaging.domain.endpoint_count")};
  ::xmotion::telemetry::Gauge g_matches_{
      ::xmotion::telemetry::GetGauge("messaging.domain.match_count")};
  std::size_t endpoint_count_ = 0;  // guarded by mutex_

  // Caller holds mutex_.
  void UpdateGauges() {
    g_endpoints_.Set(static_cast<double>(endpoint_count_));
    double matches = 0.0;
    for (const auto& [name, topic] : topics_) {
      (void)name;
      matches += static_cast<double>(
                     topic->publisher_count_.load(std::memory_order_relaxed)) *
                 static_cast<double>(topic->subscriber_count_.load(
                     std::memory_order_relaxed));
    }
    g_matches_.Set(matches);
  }
};

// D17 default key derivation (user + configured name). The cross-process
// derivation algorithm is wire-contract §6.2 TBD (P1); in-process only
// needs stable process-local identity. Defined in src/domain.cpp.
std::string DeriveIsolationKey(const std::string& configured_name);
// Process-global registry lookup (weak-ref: state dies with its last
// Domain). Defined in src/domain.cpp.
std::shared_ptr<DomainState> AcquireDomainState(const std::string& key);

// ---------------------------------------------------------------------------
// DomainImpl: the reach selection made by the factory (D12).
// ---------------------------------------------------------------------------
class DomainImpl {
 public:
  enum class Reach : std::uint8_t { kInProcess, kPosixShm, kIceoryx2, kZenoh };

  DomainImpl(Reach reach, std::shared_ptr<DomainState> state)
      : reach_(reach), state_(std::move(state)) {}

  // A backend not compiled into this build yields a Domain with no state:
  // endpoints carry kUnsupportedReach (M8-A2) — never a silent in-process
  // fallback.
  bool available() const noexcept { return state_ != nullptr; }

  const Reach reach_;
  const std::shared_ptr<DomainState> state_;
};

// ---------------------------------------------------------------------------
// Endpoint impls. The base is type-erased (what Endpoint's non-template
// surface needs); Pub/SubImpl add the typed cores and unregister in their
// destructors (D7: teardown is scope exit).
// ---------------------------------------------------------------------------
class EndpointImpl {
 public:
  EndpointImpl() = default;
  virtual ~EndpointImpl() = default;
  EndpointImpl(const EndpointImpl&) = delete;
  EndpointImpl& operator=(const EndpointImpl&) = delete;

  std::size_t MatchedCount() const noexcept {
    const TopicBase* topic = topic_.get();
    if (topic == nullptr) {
      return 0;
    }
    return is_publisher_
               ? topic->subscriber_count_.load(std::memory_order_relaxed)
               : topic->publisher_count_.load(std::memory_order_relaxed);
  }

  std::shared_ptr<DomainState> state_{};
  std::shared_ptr<TopicBase> topic_{};
  bool is_publisher_ = false;
  AdvertiseStatus advertise_status_ = AdvertiseStatus::kUnsupportedReach;
  SubscribeStatus subscribe_status_ = SubscribeStatus::kUnsupportedReach;
};

// Introspection/test seam (friend of Endpoint).
struct EndpointAccess {
  static EndpointImpl* Get(const Endpoint& endpoint) noexcept {
    return endpoint.impl_;
  }
};

template <typename T>
class PubImpl final : public EndpointImpl {
 public:
  ~PubImpl() override {
    if (core_ != nullptr && topic_ != nullptr && state_ != nullptr) {
      std::lock_guard<std::mutex> lock(state_->mutex_);
      static_cast<Topic<T>*>(topic_.get())->RemovePublisher(core_.get());
      --state_->endpoint_count_;
      state_->UpdateGauges();
    }
  }

  std::shared_ptr<PubCore<T>> core_{};
};

template <typename T>
class SubImpl final : public EndpointImpl {
 public:
  ~SubImpl() override {
    if (core_ != nullptr && topic_ != nullptr && state_ != nullptr) {
      std::lock_guard<std::mutex> lock(state_->mutex_);
      static_cast<Topic<T>*>(topic_.get())->RemoveSubscriber(core_.get());
      --state_->endpoint_count_;
      state_->UpdateGauges();
    }
  }

  std::shared_ptr<SubCore<T>> core_{};
};

}  // namespace detail

// ===========================================================================
// Template member definitions for the portable API (declared in domain.hpp
// and endpoints.hpp). Wiring verbs never throw and never spawn threads;
// hot-path verbs are allocation-free after wiring (R7).
// ===========================================================================

// ---- Domain::Advertise (D18: never throws; check handle status) -----------
template <typename T>
Publisher<T> Domain::Advertise(std::string_view topic, const Qos& qos) {
  // P0b in-process bound: fan-out copies and Sample<T> value semantics.
  static_assert(std::is_default_constructible_v<T> &&
                    std::is_copy_assignable_v<T>,
                "xmMessaging (P0b in-process): payloads must be "
                "default-constructible and copy-assignable (fan-out by copy; "
                "Sample<T> is value-semantic)");
  Publisher<T> handle;
  auto impl = std::make_unique<detail::PubImpl<T>>();
  impl->is_publisher_ = true;
  if (impl_ != nullptr && impl_->available()) {
    auto state = impl_->state_;
    std::lock_guard<std::mutex> lock(state->mutex_);
    auto& entry = state->topics_[std::string(topic)];
    const std::uint64_t hash = detail::SchemaHashOf<T>();
    if (entry == nullptr) {
      entry = std::make_shared<detail::Topic<T>>(std::string(topic), hash);
    }
    if (entry->schema_hash_ != hash || *entry->type_ != typeid(T)) {
      impl->advertise_status_ = AdvertiseStatus::kTypeMismatch;  // R6
    } else {
      auto* typed = static_cast<detail::Topic<T>*>(entry.get());
      std::shared_ptr<detail::PubCore<T>> core;
      impl->advertise_status_ = typed->AddPublisher(qos, &core);
      if (impl->advertise_status_ == AdvertiseStatus::kOk) {
        impl->core_ = std::move(core);
        impl->topic_ = entry;
        impl->state_ = state;
        ++state->endpoint_count_;
        state->UpdateGauges();  // D16: WaitUntilMatched polls the counters
      }
    }
  }
  handle.impl_ = impl.release();
  return handle;
}

// ---- Domain::Subscribe (order-independent: publisher may not exist) -------
template <typename T>
Subscriber<T> Domain::Subscribe(std::string_view topic, const Qos& qos) {
  static_assert(std::is_default_constructible_v<T> &&
                    std::is_copy_assignable_v<T>,
                "xmMessaging (P0b in-process): payloads must be "
                "default-constructible and copy-assignable (fan-out by copy; "
                "Sample<T> is value-semantic)");
  Subscriber<T> handle;
  auto impl = std::make_unique<detail::SubImpl<T>>();
  if (impl_ != nullptr && impl_->available()) {
    auto state = impl_->state_;
    std::lock_guard<std::mutex> lock(state->mutex_);
    auto& entry = state->topics_[std::string(topic)];
    const std::uint64_t hash = detail::SchemaHashOf<T>();
    if (entry == nullptr) {
      entry = std::make_shared<detail::Topic<T>>(std::string(topic), hash);
    }
    if (entry->schema_hash_ != hash || *entry->type_ != typeid(T)) {
      impl->subscribe_status_ = SubscribeStatus::kTypeMismatch;  // R6
    } else {
      auto* typed = static_cast<detail::Topic<T>*>(entry.get());
      impl->core_ = typed->AddSubscriber(qos);
      impl->topic_ = entry;
      impl->state_ = state;
      impl->subscribe_status_ = SubscribeStatus::kOk;
      ++state->endpoint_count_;
      state->UpdateGauges();  // D16: WaitUntilMatched polls the counters
    }
  }
  handle.impl_ = impl.release();
  return handle;
}

// ---- Loan<T> ----------------------------------------------------------------
template <typename T>
Loan<T>::Loan(Loan&& other) noexcept
    : slot_(other.slot_), status_(other.status_), pool_(other.pool_) {
  other.slot_ = nullptr;
  other.status_ = LoanStatus::kExhausted;
  other.pool_ = nullptr;
}

template <typename T>
Loan<T>& Loan<T>::operator=(Loan&& other) noexcept {
  if (this != &other) {
    if (pool_ != nullptr && slot_ != nullptr) {
      pool_->Release(slot_);
    }
    slot_ = other.slot_;
    status_ = other.status_;
    pool_ = other.pool_;
    other.slot_ = nullptr;
    other.status_ = LoanStatus::kExhausted;
    other.pool_ = nullptr;
  }
  return *this;
}

template <typename T>
Loan<T>::~Loan() {
  // An unpublished loan is returned to the pool at scope exit (RAII).
  if (pool_ != nullptr && slot_ != nullptr) {
    pool_->Release(slot_);
  }
}

// ---- Publisher<T> ------------------------------------------------------------
template <typename T>
Publisher<T>::Publisher(Publisher&& other) noexcept = default;
template <typename T>
Publisher<T>& Publisher<T>::operator=(Publisher&& other) noexcept = default;
template <typename T>
Publisher<T>::~Publisher() = default;

template <typename T>
AdvertiseStatus Publisher<T>::status() const noexcept {
  // A moved-from handle reads as kUnsupportedReach (it can serve nothing).
  return impl_ != nullptr
             ? static_cast<const detail::PubImpl<T>*>(impl_)->advertise_status_
             : AdvertiseStatus::kUnsupportedReach;
}

template <typename T>
Loan<T> Publisher<T>::Loan() {
  // M6-A4: the zero-copy path is a compile-time fact at the wiring site
  // (asserted here, at the minting verb — see the note in endpoints.hpp).
  static_assert(is_zero_copy_payload_v<T>,
                "xmMessaging: Loan (zero-copy publication) requires a "
                "trivially-copyable payload (design.md QoS 'Loan', M6-A4)");
  ::xmotion::messaging::Loan<T> loan;
  auto* impl = static_cast<detail::PubImpl<T>*>(impl_);
  assert(impl != nullptr &&
         impl->advertise_status_ == AdvertiseStatus::kOk &&
         "xmMessaging: Loan() on a non-kOk Publisher is a contract "
         "violation (D18)");
  if (impl == nullptr || impl->core_ == nullptr) {
    return loan;  // kExhausted
  }
  T* slot = impl->core_->pool.Acquire();
  if (slot != nullptr) {
    loan.slot_ = slot;
    loan.pool_ = &impl->core_->pool;
    loan.status_ = LoanStatus::kOk;
  }
  return loan;
}

template <typename T>
PublishStatus Publisher<T>::Publish(const T& value) {
  auto* impl = static_cast<detail::PubImpl<T>*>(impl_);
  assert(impl != nullptr &&
         impl->advertise_status_ == AdvertiseStatus::kOk &&
         "xmMessaging: Publish on a non-kOk Publisher is a contract "
         "violation (D18)");
  if (impl == nullptr || impl->core_ == nullptr || impl->topic_ == nullptr) {
    return PublishStatus::kOk;  // contract violation; safe no-op in release
  }
  auto* topic = static_cast<detail::Topic<T>*>(impl->topic_.get());
  return topic->PublishFrom(*impl->core_, value, nullptr);
}

template <typename T>
PublishStatus Publisher<T>::Publish(::xmotion::messaging::Loan<T>&& loan) {
  assert(loan.status() == LoanStatus::kOk &&
         "xmMessaging: publishing an exhausted Loan is a contract violation "
         "(D18/LoanStatus)");
  // Take ownership; RAII returns the pool cell after the copy-out below.
  ::xmotion::messaging::Loan<T> consumed(std::move(loan));
  if (consumed.slot_ == nullptr) {
    return PublishStatus::kOk;
  }
  return Publish(static_cast<const T&>(*consumed.slot_));
}

template <typename T>
template <typename... Upstream>
PublishStatus Publisher<T>::PublishDerived(
    ::xmotion::messaging::Loan<T>&& loan, const Sample<Upstream>&... upstream) {
  detail::DerivedInfo info{};
  bool has_upstream = false;
  if constexpr (sizeof...(Upstream) > 0) {
    has_upstream = true;
    // D14: origin = OLDEST consumed input's origin (information is only as
    // fresh as its stalest ingredient); hops = max upstream + 1, saturating
    // at 65535 (wire-contract §2: MUST NOT wrap).
    const std::int64_t origins[] = {
        upstream.origin_stamp().time_since_epoch().count()...};
    const std::uint32_t hops[] = {upstream.hop_count()...};
    info.origin_ns = origins[0];
    std::uint32_t max_hops = hops[0];
    for (std::size_t i = 1; i < sizeof...(Upstream); ++i) {
      if (origins[i] < info.origin_ns) {
        info.origin_ns = origins[i];
      }
      if (hops[i] > max_hops) {
        max_hops = hops[i];
      }
    }
    info.hops = max_hops >= 65535u ? 65535u : max_hops + 1u;
  }

  auto* impl = static_cast<detail::PubImpl<T>*>(impl_);
  assert(impl != nullptr &&
         impl->advertise_status_ == AdvertiseStatus::kOk &&
         "xmMessaging: PublishDerived on a non-kOk Publisher is a contract "
         "violation (D18)");
  ::xmotion::messaging::Loan<T> consumed(std::move(loan));
  if (impl == nullptr || impl->core_ == nullptr || impl->topic_ == nullptr ||
      consumed.slot_ == nullptr) {
    return PublishStatus::kOk;
  }
  auto* topic = static_cast<detail::Topic<T>*>(impl->topic_.get());
  return topic->PublishFrom(*impl->core_, *consumed.slot_,
                            has_upstream ? &info : nullptr);
}

// ---- Subscriber<T> -------------------------------------------------------
template <typename T>
Subscriber<T>::Subscriber(Subscriber&& other) noexcept = default;
template <typename T>
Subscriber<T>& Subscriber<T>::operator=(Subscriber&& other) noexcept = default;
template <typename T>
Subscriber<T>::~Subscriber() = default;

template <typename T>
SubscribeStatus Subscriber<T>::status() const noexcept {
  return impl_ != nullptr
             ? static_cast<const detail::SubImpl<T>*>(impl_)->subscribe_status_
             : SubscribeStatus::kUnsupportedReach;
}

template <typename T>
Sample<T> Subscriber<T>::TakeLatest() {
  Sample<T> sample;  // kNone until proven otherwise (D1/D2)
  auto* impl = static_cast<detail::SubImpl<T>*>(impl_);
  if (impl == nullptr || impl->core_ == nullptr) {
    assert(impl != nullptr && "xmMessaging: TakeLatest on a moved-from "
                              "Subscriber is a contract violation");
    return sample;
  }
  detail::SubCore<T>& core = *impl->core_;
  assert(core.latest != nullptr &&
         "xmMessaging: TakeLatest is the latest-only take verb (D1); this "
         "subscriber declared queue history — use TryTake");
  if (core.latest == nullptr) {
    return sample;
  }

  detail::MailRecord<T> record;
  if (!core.latest->Load(record)) {
    return sample;  // kNone: never received (M14-A1)
  }

  sample.value_ = record.payload;
  sample.stamp_ = Timestamp(Duration(record.envelope.publish_stamp_ns));
  sample.origin_stamp_ = Timestamp(Duration(record.envelope.origin_stamp_ns));
  sample.hop_count_ = record.envelope.hop_count;
  sample.context_ = ::xmotion::telemetry::Extract(
      record.envelope.context, detail::kEnvelopeContextSize);
  sample.age_class_ = AgeClass::kMeasured;  // D12: one process, one clock

  // D2/D3: freshness judged here against the wiring-time deadline.
  const Duration age = ::xmotion::Now() - sample.stamp_;
  const bool stale = core.deadline.has_value() && age > *core.deadline;
  sample.freshness_ = stale ? Freshness::kStale : Freshness::kFresh;

  // Counters (D9) + instruments (R11 §7). Allocation-free atomic updates.
  core.take_count.fetch_add(1, std::memory_order_relaxed);
  core.t_take.Add();
  core.t_take_age.Record(
      static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(age)
                              .count()));
  const std::uint64_t last =
      core.last_consumed_ordinal.load(std::memory_order_relaxed);
  if (record.ordinal > last) {
    const std::uint64_t gap = record.ordinal - last - 1;
    if (gap > 0) {
      // LatestMailbox guarantee 3, materialized at take (see header).
      core.overwrite_count.fetch_add(gap, std::memory_order_relaxed);
      core.t_overwrite.Add(static_cast<double>(gap));
    }
    core.last_consumed_ordinal.store(record.ordinal,
                                     std::memory_order_relaxed);
  }
  if (stale) {
    // D3: one deadline-miss event per Fresh->Stale transition.
    if (!core.stale_latched.exchange(true, std::memory_order_relaxed)) {
      core.deadline_miss_count.fetch_add(1, std::memory_order_relaxed);
      core.t_deadline_miss.Add();
    }
  } else {
    core.stale_latched.store(false, std::memory_order_relaxed);
  }
  return sample;
}

template <typename T>
Sample<T> Subscriber<T>::TryTake() {
  Sample<T> sample;  // kNone when empty (D5: never blocks)
  auto* impl = static_cast<detail::SubImpl<T>*>(impl_);
  if (impl == nullptr || impl->core_ == nullptr) {
    assert(impl != nullptr && "xmMessaging: TryTake on a moved-from "
                              "Subscriber is a contract violation");
    return sample;
  }
  detail::SubCore<T>& core = *impl->core_;
  assert(core.queue != nullptr &&
         "xmMessaging: TryTake is the queue take verb; this subscriber "
         "declared latest-only history — use TakeLatest");
  if (core.queue == nullptr) {
    return sample;
  }

  detail::MailRecord<T> record;
  if (!core.queue->TryPop(record)) {
    return sample;  // kNone: empty
  }
  core.t_queue_depth.Set(static_cast<double>(core.queue->Size()));

  sample.value_ = record.payload;
  sample.stamp_ = Timestamp(Duration(record.envelope.publish_stamp_ns));
  sample.origin_stamp_ = Timestamp(Duration(record.envelope.origin_stamp_ns));
  sample.hop_count_ = record.envelope.hop_count;
  sample.context_ = ::xmotion::telemetry::Extract(
      record.envelope.context, detail::kEnvelopeContextSize);
  sample.age_class_ = AgeClass::kMeasured;

  const Duration age = ::xmotion::Now() - sample.stamp_;
  const bool stale = core.deadline.has_value() && age > *core.deadline;
  sample.freshness_ = stale ? Freshness::kStale : Freshness::kFresh;

  core.take_count.fetch_add(1, std::memory_order_relaxed);
  core.t_take.Add();
  core.t_take_age.Record(
      static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(age)
                              .count()));
  if (stale) {
    if (!core.stale_latched.exchange(true, std::memory_order_relaxed)) {
      core.deadline_miss_count.fetch_add(1, std::memory_order_relaxed);
      core.t_deadline_miss.Add();
    }
  } else {
    core.stale_latched.store(false, std::memory_order_relaxed);
  }
  return sample;
}

// ---- introspect (D9) -------------------------------------------------------
namespace introspect {

template <typename T>
std::uint64_t DropCount(const Subscriber<T>& subscriber) {
  auto* impl = static_cast<detail::SubImpl<T>*>(
      detail::EndpointAccess::Get(subscriber));
  return (impl != nullptr && impl->core_ != nullptr)
             ? impl->core_->drop_count.load(std::memory_order_relaxed)
             : 0;
}

template <typename T>
std::uint64_t OverwriteCount(const Subscriber<T>& subscriber) {
  auto* impl = static_cast<detail::SubImpl<T>*>(
      detail::EndpointAccess::Get(subscriber));
  return (impl != nullptr && impl->core_ != nullptr)
             ? impl->core_->overwrite_count.load(std::memory_order_relaxed)
             : 0;
}

template <typename T>
std::uint64_t TakeCount(const Subscriber<T>& subscriber) {
  auto* impl = static_cast<detail::SubImpl<T>*>(
      detail::EndpointAccess::Get(subscriber));
  return (impl != nullptr && impl->core_ != nullptr)
             ? impl->core_->take_count.load(std::memory_order_relaxed)
             : 0;
}

template <typename T>
std::uint64_t PublishCount(const Publisher<T>& publisher) {
  auto* impl =
      static_cast<detail::PubImpl<T>*>(detail::EndpointAccess::Get(publisher));
  return (impl != nullptr && impl->core_ != nullptr)
             ? impl->core_->publish_count.load(std::memory_order_relaxed)
             : 0;
}

template <typename T>
std::uint64_t RefusedCount(const Publisher<T>& publisher) {
  auto* impl =
      static_cast<detail::PubImpl<T>*>(detail::EndpointAccess::Get(publisher));
  return (impl != nullptr && impl->core_ != nullptr)
             ? impl->core_->refused_count.load(std::memory_order_relaxed)
             : 0;
}

}  // namespace introspect

}  // namespace messaging
}  // namespace xmotion

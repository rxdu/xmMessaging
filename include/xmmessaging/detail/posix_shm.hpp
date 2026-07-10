/*
 * detail/posix_shm.hpp
 *
 * The POSIX shm fallback backend (P1b) — the dependency-free inter-process
 * reach (design.md "The POSIX shm fallback backend"). Kernel-native
 * primitives only: named shm segments (shm_segment.hpp) + the SAME slot and
 * ring algorithms the in-process reach runs, re-placed into the shared
 * mapping (latest_slot.hpp / bounded_queue.hpp over ShmRegionPlacement) —
 * the placement parameterization bet, cashed in: zero algorithm changes.
 *
 * Honestly-partial support matrix (R3 divergence-over-emulation, M6-A6),
 * decided and queryable via Domain::Supports():
 *
 *   kLatestOnly        YES — writer-progress-only seqlock slot per subscriber
 *   kBoundedQueue      YES — per-subscriber shared SPSC ring, depth <= 16
 *   kLateJoinWarmStart YES — the master slot lives in the segment; it even
 *                            survives publisher death (M2 across M4)
 *   kDeadline          YES — same host, one CLOCK_MONOTONIC (R8): kMeasured
 *   kReliableQueue     NO  — cross-process all-or-nothing back-pressure
 *                            needs a pre-check spanning peer-owned rings;
 *                            deferred, declared (Advertise refuses)
 *   kZeroCopyLoan      NO  — the seqlock transports records THROUGH atomic
 *                            words (that is what makes reads crash-safe), so
 *                            publication is inherently a copy; Loan() the
 *                            VERB still works (scratch-cell construct +
 *                            copying publish) but the zero-copy contract is
 *                            a declared divergence (iceoryx2 is the
 *                            zero-copy backend)
 *   kRequestResponse   NO  — later or declared divergence (design.md);
 *                            Serve/Client return kUnsupportedReach
 *   kSharedOwnership   NO  — two unserialized writers would corrupt the
 *                            seqlock; in-process serializes with a mutex,
 *                            and a cross-process mutex is exactly the
 *                            robust-lock machinery this backend exists to
 *                            avoid (design.md). Advertise refuses kShared.
 *
 * Unsupported-contract wiring refusals surface as kUnsupportedReach (D19:
 * per-reach requirements are wiring STATUSES; it is the one status that
 * says "this reach cannot serve this request" — M8-A2's meaning, extended
 * per the honestly-partial matrix).
 *
 * Hot-path discipline (R7): after wiring, Publish/TakeLatest/TryTake touch
 * only the mapped segment — no syscalls, no allocation, no locks; publish
 * is wait-free (bounded 16-slot fan-out), take is lock-free (bounded
 * seqlock retries with a crash-detecting budget). Every pid/liveness check
 * (kill(pid, 0)) happens on wiring or readiness paths only.
 *
 * Crash story (M4): see shm_segment.hpp (liveness slots, ordinal
 * continuation) and latest_slot.hpp (LoadBounded/RepairAfterWriterCrash).
 * The subscriber additionally keeps its last successfully-taken record as
 * a process-local fallback, so a publisher SIGKILLed mid-store degrades to
 * rising staleness on the value already held — never a torn read, never a
 * block, never a kNone regression (M4-A1).
 *
 * detail/: not part of the portable API surface. Included by in_process.hpp
 * only (it needs EndpointImpl/LoanPool/DerivedInfo from there); gated on
 * XMMESSAGING_HAS_POSIX_SHM.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/handles.hpp"
#include "xmmessaging/detail/bounded_queue.hpp"
#include "xmmessaging/detail/envelope.hpp"
#include "xmmessaging/detail/latest_slot.hpp"
#include "xmmessaging/detail/mail_record.hpp"
#include "xmmessaging/detail/placement.hpp"
#include "xmmessaging/detail/schema_hash.hpp"
#include "xmmessaging/detail/shm_segment.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// Seqlock retry budget before a read is declared stalled (dead writer or
// pathological overwrite pressure) and the caller falls back to its cached
// record. Each retry implies writer progress, so 4096 consecutive
// overwrites during one read is not a plausible live-writer profile.
inline constexpr std::uint32_t kShmSeqlockRetryBudget = 4096;

// ---------------------------------------------------------------------------
// ShmTopicAttachment<T>: one process's typed view of a topic segment.
// The layout is a pure function of T and the segment constants, so every
// process computes identical offsets — no negotiation, no fd passing.
//
// Segment map (all region starts 64-aligned; total validated in the header):
//   [ ShmSegmentHeader ]
//   [ master latest slot ]                      (warm start, D6/M2)
//   [ sub region 0: latest slot | ring control | ring cells x16 ]
//   ...
//   [ sub region 15 ]
// ---------------------------------------------------------------------------
template <typename T>
class ShmTopicAttachment {
 public:
  using Record = MailRecord<T>;
  using Slot = LatestSlot<T, ShmRegionPlacement>;
  using Ring = BoundedQueue<T, ShmRegionPlacement>;

  static constexpr std::size_t Align64(std::size_t v) noexcept {
    return (v + 63u) & ~std::size_t{63u};
  }
  static constexpr std::size_t LatestRegionBytes() noexcept {
    return Align64(Slot::StorageBytes());
  }
  static constexpr std::size_t RingRegionBytes() noexcept {
    return Align64(Align64(Ring::ControlBytes()) +
                   std::size_t{kShmRingCapacity} * Ring::RecordBytes());
  }
  static constexpr std::size_t SubRegionStride() noexcept {
    return LatestRegionBytes() + RingRegionBytes();
  }
  static constexpr std::size_t MasterOffset() noexcept {
    return Align64(sizeof(ShmSegmentHeader));
  }
  static constexpr std::size_t SubRegionOffset(std::uint32_t index) noexcept {
    return MasterOffset() + LatestRegionBytes() +
           std::size_t{index} * SubRegionStride();
  }
  static constexpr std::size_t TotalBytes() noexcept {
    return SubRegionOffset(kShmMaxSubscribers);
  }

  static ShmAttachStatus Attach(const std::string& name,
                                std::uint64_t schema_hash, const Qos& qos,
                                std::unique_ptr<ShmTopicAttachment>* out) {
    ShmMapping mapping;
    const ShmAttachStatus status =
        ShmMapping::OpenOrCreate(name, schema_hash, sizeof(T), alignof(T),
                                 TotalBytes(), qos, &mapping);
    if (status != ShmAttachStatus::kOk) {
      return status;
    }
    auto attachment =
        std::make_unique<ShmTopicAttachment>(std::move(mapping));
    if (attachment->mapping_.created()) {
      // Data plane initialized (master slot zeroed by its placement above;
      // sub regions are untouched zero pages == all slots kShmSubFree):
      // publish the segment to attachers.
      attachment->mapping_.MarkReady();
    }
    *out = std::move(attachment);
    return ShmAttachStatus::kOk;
  }

  explicit ShmTopicAttachment(ShmMapping mapping)
      : mapping_(std::move(mapping)),
        master_(ShmRegionPlacement(mapping_.base() + MasterOffset(),
                                   LatestRegionBytes(), mapping_.created())) {
    // Attach views over every sub region (initialize = false: the OWNING
    // subscriber initializes its region at claim time). Built at wiring so
    // the fan-out never constructs anything (R7).
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      latest_views_[i] = std::make_unique<Slot>(ShmRegionPlacement(
          SubLatestBase(i), LatestRegionBytes(), false));
      ring_views_[i] = std::make_unique<Ring>(
          kShmRingCapacity,
          ShmRegionPlacement(SubRingBase(i), RingRegionBytes(), false));
    }
  }

  ShmSegmentHeader* header() const noexcept { return mapping_.header(); }
  Slot& master() noexcept { return master_; }
  Slot& sub_latest(std::uint32_t index) noexcept {
    return *latest_views_[index];
  }
  Ring& sub_ring(std::uint32_t index) noexcept { return *ring_views_[index]; }

  unsigned char* SubLatestBase(std::uint32_t index) const noexcept {
    return mapping_.base() + SubRegionOffset(index);
  }
  unsigned char* SubRingBase(std::uint32_t index) const noexcept {
    return mapping_.base() + SubRegionOffset(index) + LatestRegionBytes();
  }

 private:
  ShmMapping mapping_;
  Slot master_;
  std::unique_ptr<Slot> latest_views_[kShmMaxSubscribers];
  std::unique_ptr<Ring> ring_views_[kShmMaxSubscribers];
};

// ---------------------------------------------------------------------------
// Publisher endpoint impl.
// ---------------------------------------------------------------------------
template <typename T>
class ShmPubImpl final : public EndpointImpl {
 public:
  using Record = MailRecord<T>;

  ShmPubImpl(const Qos& qos, const std::string& topic)
      : n_publish("messaging.pub." + topic + ".publish_count"),
        n_refused("messaging.pub." + topic + ".refused_count"),
        n_bytes("messaging.pub." + topic + ".bytes"),
        t_publish(::xmotion::telemetry::GetCounter(n_publish)),
        t_refused(::xmotion::telemetry::GetCounter(n_refused)),
        t_bytes(::xmotion::telemetry::GetCounter(n_bytes)) {
    (void)qos;
  }

  ~ShmPubImpl() override {
    if (attachment_ != nullptr && owns_publisher_slot_) {
      // Graceful release: the next Advertise claims a zero slot instead of
      // needing the ESRCH reclaim path. A SIGKILLed publisher skips this,
      // which is exactly what the reclaim path exists for (M4-A2).
      auto* h = attachment_->header();
      std::uint32_t me = static_cast<std::uint32_t>(::getpid());
      h->pub_pid.compare_exchange_strong(me, 0u, std::memory_order_acq_rel,
                                         std::memory_order_relaxed);
    }
  }

  // D16 readiness verb (wiring/launcher path — the liveness probe is a
  // syscall per active slot, deliberately NOT hot-path; M4-A4: a dead
  // subscriber drops out of the count here).
  std::size_t MatchedCount() const noexcept override {
    if (attachment_ == nullptr) {
      return 0;
    }
    auto* h = attachment_->header();
    std::size_t count = 0;
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      const ShmSubSlot& slot = h->sub_slots[i];
      if (slot.state.load(std::memory_order_acquire) == kShmSubActive &&
          ProcessAlive(slot.pid.load(std::memory_order_relaxed))) {
        ++count;
      }
    }
    return count;
  }

  // D15 exclusive claim over the segment's publisher-liveness slot, with
  // the M4-A2 dead-owner reclaim. Wiring path only.
  AdvertiseStatus ClaimPublisherSlot() {
    auto* h = attachment_->header();
    const std::uint32_t me = static_cast<std::uint32_t>(::getpid());
    std::uint32_t expected = 0;
    if (!h->pub_pid.compare_exchange_strong(expected, me,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      if (ProcessAlive(expected)) {
        // Live owner (possibly this very process — a duplicate Advertise
        // is refused the same way, M14-A3).
        return AdvertiseStatus::kOwnershipRefused;
      }
      if (!h->pub_pid.compare_exchange_strong(expected, me,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return AdvertiseStatus::kOwnershipRefused;  // lost the reclaim race
      }
    }
    owns_publisher_slot_ = true;
    h->pub_epoch.fetch_add(1, std::memory_order_relaxed);  // generation (M4-A4)
    // Sole writer now: repair any seqlock cell the previous owner poisoned
    // by dying mid-store (master + every active latest mailbox). Ordinals
    // resume from header->accepted_ordinal, which survived in the segment —
    // the restarted-writer story documented in shm_segment.hpp.
    attachment_->master().RepairAfterWriterCrash();
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      const ShmSubSlot& slot = h->sub_slots[i];
      if (slot.state.load(std::memory_order_acquire) == kShmSubActive &&
          slot.history_kind.load(std::memory_order_relaxed) == 0u) {
        attachment_->sub_latest(i).RepairAfterWriterCrash();
      }
    }
    return AdvertiseStatus::kOk;
  }

  // The hot path. Wait-free: bounded straight-line fan-out, no syscalls,
  // no allocation (R7). derived == nullptr for a first-hop publish.
  PublishStatus Publish(const T& value, const DerivedInfo* derived) {
    auto* h = attachment_->header();
    Record record;
    const auto context_bytes =
        ::xmotion::telemetry::Inject(::xmotion::telemetry::CurrentContext());
    std::memcpy(record.envelope.context, context_bytes.data(),
                kEnvelopeContextSize);
    const std::int64_t now_ns = ::xmotion::Now().time_since_epoch().count();
    record.envelope.publish_stamp_ns = now_ns;
    record.envelope.origin_stamp_ns =
        derived != nullptr ? derived->origin_ns : now_ns;  // D14
    record.envelope.hop_count = derived != nullptr ? derived->hops : 0;
    record.envelope.flags = 0;
    // The segment authors the ordinals: contiguous over accepted publishes
    // ACROSS publisher restarts (M4-A2), keeping gap accounting exact.
    record.ordinal =
        h->accepted_ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
    record.payload = value;

    attachment_->master().Store(record);  // D6 warm-start source
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      ShmSubSlot& slot = h->sub_slots[i];
      if (slot.state.load(std::memory_order_acquire) != kShmSubActive) {
        continue;
      }
      if (slot.history_kind.load(std::memory_order_relaxed) == 0u) {
        // D6 publisher-stamped baseline, verbatim from the in-process
        // Deliver (see the long rationale there): the first delivery into
        // a fresh mailbox stamps "everything before this record is
        // pre-join", now through a shared atomic. The mailbox's seqlock
        // release/acquire pair publishes baseline and record together.
        if (slot.last_consumed_ordinal.load(std::memory_order_relaxed) ==
            kShmBaselineUnset) {
          std::uint64_t expected = kShmBaselineUnset;
          slot.last_consumed_ordinal.compare_exchange_strong(
              expected, record.ordinal - 1, std::memory_order_relaxed,
              std::memory_order_relaxed);
        }
        attachment_->sub_latest(i).Store(record);  // overwrite, never refuse
      } else {
        if (!attachment_->sub_ring(i).TryPush(record)) {
          // best-effort drop-newest: that subscriber's fact (D8), counted
          // in ITS shared slot so conservation reconciles cross-process
          // (M3-A2). The subscriber's telemetry instrument is emitted by
          // the subscriber at take time from this counter's delta (a
          // publisher cannot Add() to another process's instruments).
          slot.drop_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    publish_count.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(sizeof(T), std::memory_order_relaxed);
    h->pub_publish_count.fetch_add(1, std::memory_order_relaxed);
    h->pub_bytes.fetch_add(sizeof(T), std::memory_order_relaxed);
    t_publish.Add();
    t_bytes.Add(static_cast<double>(sizeof(T)));
    return PublishStatus::kOk;
  }

  std::unique_ptr<ShmTopicAttachment<T>> attachment_;
  bool owns_publisher_slot_ = false;

  // R11 §7 instruments (same names/divergences as the in-process PubCore).
  const std::string n_publish, n_refused, n_bytes;
  ::xmotion::telemetry::Counter t_publish, t_refused, t_bytes;

  // D9 endpoint-lifetime counters (process-local, matching in-process
  // semantics; the segment's pub_publish_count is cumulative across
  // publisher generations and serves observability).
  std::atomic<std::uint64_t> publish_count{0};
  std::atomic<std::uint64_t> refused_count{0};
  std::atomic<std::uint64_t> bytes{0};

  LoanPool<T> pool;
};

// ---------------------------------------------------------------------------
// Subscriber endpoint impl.
// ---------------------------------------------------------------------------
template <typename T>
class ShmSubImpl final : public EndpointImpl {
 public:
  using Slot = typename ShmTopicAttachment<T>::Slot;
  using Ring = typename ShmTopicAttachment<T>::Ring;

  ShmSubImpl(const Qos& qos, const std::string& topic)
      : history_(qos.history),
        deadline_(qos.deadline),
        n_take("messaging.sub." + topic + ".take_count"),
        n_drop("messaging.sub." + topic + ".drop_count"),
        n_overwrite("messaging.sub." + topic + ".overwrite_count"),
        n_deadline_miss("messaging.sub." + topic + ".deadline_miss_count"),
        n_take_age("messaging.sub." + topic + ".take_age_us"),
        n_queue_depth("messaging.sub." + topic + ".queue_depth"),
        n_hop_latency("messaging.hop." + topic + ".hop_latency_us"),
        t_take(::xmotion::telemetry::GetCounter(n_take)),
        t_drop(::xmotion::telemetry::GetCounter(n_drop)),
        t_overwrite(::xmotion::telemetry::GetCounter(n_overwrite)),
        t_deadline_miss(::xmotion::telemetry::GetCounter(n_deadline_miss)),
        t_take_age(::xmotion::telemetry::GetHistogram(n_take_age)),
        t_queue_depth(::xmotion::telemetry::GetGauge(n_queue_depth)),
        t_hop_latency(::xmotion::telemetry::GetHistogram(n_hop_latency)) {}

  ~ShmSubImpl() override {
    if (attachment_ != nullptr && slot_index_ >= 0) {
      ShmSubSlot& slot = attachment_->header()->sub_slots[slot_index_];
      slot.state.store(kShmSubFree, std::memory_order_release);
      slot.pid.store(0, std::memory_order_relaxed);
    }
  }

  std::size_t MatchedCount() const noexcept override {
    if (attachment_ == nullptr) {
      return 0;
    }
    // Readiness/wiring verb: liveness probe allowed here, never on take
    // (M4-A4: a SIGKILLed publisher drops this to 0; rejoin raises it).
    const std::uint32_t pid =
        attachment_->header()->pub_pid.load(std::memory_order_acquire);
    return ProcessAlive(pid) ? 1 : 0;
  }

  // Claim a subscriber slot: free slots first, then dead-owner reclaim.
  // Wiring path only. Returns false when all 16 are held by live owners
  // (the documented per-topic bound).
  bool ClaimSubscriberSlot() {
    auto* h = attachment_->header();
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      ShmSubSlot& slot = h->sub_slots[i];
      std::uint32_t expected = kShmSubFree;
      bool claimed = slot.state.compare_exchange_strong(
          expected, kShmSubClaimed, std::memory_order_acq_rel,
          std::memory_order_relaxed);
      if (!claimed && expected == kShmSubActive &&
          !ProcessAlive(slot.pid.load(std::memory_order_relaxed))) {
        // Dead-owner reclaim (M4: dead subscriber slots are reclaimable).
        claimed = slot.state.compare_exchange_strong(
            expected, kShmSubClaimed, std::memory_order_acq_rel,
            std::memory_order_relaxed);
        if (claimed) {
          // A live publisher may have read this slot as ACTIVE before our
          // CAS and still be mid-fan-out into the region we are about to
          // re-initialize — a writer-writer window on the seqlock. Wait it
          // out (wiring path): two ordinal advances prove the racing
          // fan-out completed (the publisher is sequential); an idle
          // publisher cannot have been mid-fan-out for the bound below.
          if (ProcessAlive(h->pub_pid.load(std::memory_order_acquire))) {
            const std::uint64_t start =
                h->accepted_ordinal.load(std::memory_order_acquire);
            for (int spin = 0; spin < 20; ++spin) {
              if (h->accepted_ordinal.load(std::memory_order_acquire) >=
                  start + 2) {
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          }
        }
      }
      if (!claimed) {
        continue;
      }
      // Exclusive owner of the region between kClaimed and kActive.
      slot.pid.store(static_cast<std::uint32_t>(::getpid()),
                     std::memory_order_relaxed);
      const bool latest = history_.kind() == History::Kind::kLatestOnly;
      slot.history_kind.store(latest ? 0u : 1u, std::memory_order_relaxed);
      slot.queue_depth.store(history_.depth(), std::memory_order_relaxed);
      slot.last_consumed_ordinal.store(kShmBaselineUnset,
                                       std::memory_order_relaxed);
      slot.take_count.store(0, std::memory_order_relaxed);
      slot.drop_count.store(0, std::memory_order_relaxed);
      slot.overwrite_count.store(0, std::memory_order_relaxed);
      slot.deadline_miss_count.store(0, std::memory_order_relaxed);
      if (latest) {
        // (Re)initialize the mailbox, then seed it from the master slot —
        // D6 warm start with the ORIGINAL envelope, exactly the in-process
        // AddSubscriber sequence: seed BEFORE the activation store below,
        // because the seqlock is single-writer and the moment kShmSubActive
        // lands a racing publish may write this mailbox.
        my_latest_ = std::make_unique<Slot>(ShmRegionPlacement(
            attachment_->SubLatestBase(i),
            ShmTopicAttachment<T>::LatestRegionBytes(), true));
        MailRecord<T> current;
        if (attachment_->master().LoadBounded(current,
                                              kShmSeqlockRetryBudget) ==
            Slot::LoadResult::kValue) {
          my_latest_->Store(current);
        }
      } else {
        my_ring_ = std::make_unique<Ring>(
            history_.depth() == 0 ? 1 : history_.depth(),
            ShmRegionPlacement(attachment_->SubRingBase(i),
                               ShmTopicAttachment<T>::RingRegionBytes(),
                               true));
      }
      slot.state.store(kShmSubActive, std::memory_order_release);
      slot_index_ = static_cast<int>(i);
      return true;
    }
    return false;  // per-topic subscriber bound reached (documented)
  }

  // The latest-only take verb (hot path: lock-free, allocation-free, no
  // syscalls — R7). Mirrors the in-process TakeLatest accounting exactly,
  // with the D6/D9 state in shared atomics and the M4-A1 cache fallback.
  Sample<T> TakeLatestSample() {
    Sample<T> sample;  // kNone until proven otherwise (D1/D2)
    assert(my_latest_ != nullptr &&
           "xmMessaging: TakeLatest is the latest-only take verb (D1); this "
           "subscriber declared queue history — use TryTake");
    if (my_latest_ == nullptr || slot_index_ < 0) {
      return sample;
    }
    ShmSubSlot& slot = attachment_->header()->sub_slots[slot_index_];

    MailRecord<T> record;
    bool have = false;
    switch (my_latest_->LoadBounded(record, kShmSeqlockRetryBudget)) {
      case Slot::LoadResult::kValue:
        cached_ = record;  // the M4-A1 fallback for later takes
        has_cache_ = true;
        have = true;
        break;
      case Slot::LoadResult::kEmpty:    // never written, or crash-repaired
      case Slot::LoadResult::kStalled:  // writer died mid-store (M4)
        if (has_cache_) {
          record = cached_;  // the value this subscriber already held —
          have = true;       // staleness keeps rising from its true stamp
        }
        break;
    }
    if (!have) {
      return sample;  // kNone: never received anything (M14-A1)
    }
    FillSample(sample, record, slot);

    const std::uint64_t last =
        slot.last_consumed_ordinal.load(std::memory_order_relaxed);
    // Same sentinel logic as in-process: a take that only ever saw the
    // warm-start seed (baseline still unset) returns it uncharged and
    // leaves the baseline for the publisher to stamp (D6).
    if (record.ordinal > last) {
      const std::uint64_t gap = record.ordinal - last - 1;
      if (gap > 0) {
        slot.overwrite_count.fetch_add(gap, std::memory_order_relaxed);
        t_overwrite.Add(static_cast<double>(gap));
      }
      slot.last_consumed_ordinal.store(record.ordinal,
                                       std::memory_order_relaxed);
    }
    return sample;
  }

  // The queue take verb (hot path, R7). SPSC pop from this subscriber's
  // shared ring; never blocks (D5).
  Sample<T> TryTakeSample() {
    Sample<T> sample;
    assert(my_ring_ != nullptr &&
           "xmMessaging: TryTake is the queue take verb; this subscriber "
           "declared latest-only history — use TakeLatest");
    if (my_ring_ == nullptr || slot_index_ < 0) {
      return sample;
    }
    ShmSubSlot& slot = attachment_->header()->sub_slots[slot_index_];
    EmitDropDelta(slot);  // publisher-counted drops -> this side's telemetry

    MailRecord<T> record;
    if (!my_ring_->TryPop(record)) {
      return sample;  // kNone: empty
    }
    t_queue_depth.Set(static_cast<double>(my_ring_->Size()));
    FillSample(sample, record, slot);
    return sample;
  }

  // D9 shared-atomic counter reads (also the cross-process introspection
  // surface: any process mapping the segment reads the same atomics).
  std::uint64_t TakeCountNow() const { return ReadCounter(&ShmSubSlot::take_count); }
  std::uint64_t DropCountNow() const { return ReadCounter(&ShmSubSlot::drop_count); }
  std::uint64_t OverwriteCountNow() const {
    return ReadCounter(&ShmSubSlot::overwrite_count);
  }

  std::unique_ptr<ShmTopicAttachment<T>> attachment_;
  int slot_index_ = -1;

 private:
  std::uint64_t ReadCounter(
      std::atomic<std::uint64_t> ShmSubSlot::*field) const {
    if (attachment_ == nullptr || slot_index_ < 0) {
      return 0;
    }
    return (attachment_->header()->sub_slots[slot_index_].*field)
        .load(std::memory_order_relaxed);
  }

  void FillSample(Sample<T>& sample, const MailRecord<T>& record,
                  ShmSubSlot& slot) {
    sample.value_ = record.payload;
    sample.stamp_ = Timestamp(Duration(record.envelope.publish_stamp_ns));
    sample.origin_stamp_ =
        Timestamp(Duration(record.envelope.origin_stamp_ns));
    sample.hop_count_ = record.envelope.hop_count;
    sample.context_ = ::xmotion::telemetry::Extract(record.envelope.context,
                                                    kEnvelopeContextSize);
    // Same host, one CLOCK_MONOTONIC by construction (R8): measured.
    sample.age_class_ = AgeClass::kMeasured;

    const Duration age = ::xmotion::Now() - sample.stamp_;
    const bool stale = deadline_.has_value() && age > *deadline_;
    sample.freshness_ = stale ? Freshness::kStale : Freshness::kFresh;

    slot.take_count.fetch_add(1, std::memory_order_relaxed);
    t_take.Add();
    const double age_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(age).count());
    t_take_age.Record(age_us);
    t_hop_latency.Record(age_us);  // §7 per-hop: same-host shared clock
    if (stale) {
      // D3: one deadline-miss event per Fresh->Stale transition.
      if (!stale_latched_.exchange(true, std::memory_order_relaxed)) {
        slot.deadline_miss_count.fetch_add(1, std::memory_order_relaxed);
        t_deadline_miss.Add();
      }
    } else {
      stale_latched_.store(false, std::memory_order_relaxed);
    }
  }

  // Cross-process telemetry split (documented in Publish above): drops are
  // COUNTED by the publisher into this slot's shared atomic; the take side
  // emits the instrument from the delta so the subscriber's process owns
  // its own messaging.sub.* instruments, same as every other reach.
  void EmitDropDelta(const ShmSubSlot& slot) {
    const std::uint64_t now =
        slot.drop_count.load(std::memory_order_relaxed);
    if (now > drops_emitted_) {
      t_drop.Add(static_cast<double>(now - drops_emitted_));
      drops_emitted_ = now;
    }
  }

  const History history_;
  const std::optional<Duration> deadline_;

  const std::string n_take, n_drop, n_overwrite, n_deadline_miss, n_take_age,
      n_queue_depth, n_hop_latency;
  ::xmotion::telemetry::Counter t_take, t_drop, t_overwrite, t_deadline_miss;
  ::xmotion::telemetry::Histogram t_take_age;
  ::xmotion::telemetry::Gauge t_queue_depth;
  ::xmotion::telemetry::Histogram t_hop_latency;

  std::unique_ptr<Slot> my_latest_;
  std::unique_ptr<Ring> my_ring_;
  MailRecord<T> cached_{};
  bool has_cache_ = false;
  std::atomic<bool> stale_latched_{false};
  std::uint64_t drops_emitted_ = 0;
};

// ---------------------------------------------------------------------------
// Wiring entry points, called from Domain::Advertise/Subscribe (which mint
// the handles — endpoint constructors stay Domain-private). Only
// instantiated for trivially copyable payloads; Domain routes other types
// to a plain kUnsupportedReach impl (the shm payload floor is a per-reach
// requirement, hence a wiring STATUS, not a compile error — D19/M6-A4).
// ---------------------------------------------------------------------------
template <typename T>
std::unique_ptr<EndpointImpl> ShmAdvertiseImpl(const std::string& key,
                                               std::string_view topic,
                                               const Qos& qos) {
  auto impl = std::make_unique<ShmPubImpl<T>>(qos, std::string(topic));
  impl->is_publisher_ = true;
  impl->backend_ = EndpointBackend::kPosixShm;
  // Honestly-partial matrix: refusals BEFORE any segment is touched.
  if (qos.reliability == Reliability::kReliable &&
      qos.history.kind() == History::Kind::kQueue) {
    return impl;  // kUnsupportedReach: reliable queues are a declared divergence
  }
  if (qos.ownership == Ownership::kShared) {
    return impl;  // kUnsupportedReach: shared ownership is a declared divergence
  }
  std::unique_ptr<ShmTopicAttachment<T>> attachment;
  switch (ShmTopicAttachment<T>::Attach(ShmSegmentName(key, topic),
                                        SchemaHashOf<T>(), qos, &attachment)) {
    case ShmAttachStatus::kOk:
      break;
    case ShmAttachStatus::kTypeMismatch:
      impl->advertise_status_ = AdvertiseStatus::kTypeMismatch;  // R6
      return impl;
    case ShmAttachStatus::kUnavailable:
      return impl;  // kUnsupportedReach (OS refusal / orphaned segment)
  }
  impl->attachment_ = std::move(attachment);
  impl->advertise_status_ = impl->ClaimPublisherSlot();
  if (impl->advertise_status_ != AdvertiseStatus::kOk) {
    impl->attachment_.reset();  // a refused handle serves nothing (D18)
  }
  return impl;
}

template <typename T>
std::unique_ptr<EndpointImpl> ShmSubscribeImpl(const std::string& key,
                                               std::string_view topic,
                                               const Qos& qos) {
  auto impl = std::make_unique<ShmSubImpl<T>>(qos, std::string(topic));
  impl->backend_ = EndpointBackend::kPosixShm;
  if (qos.history.kind() == History::Kind::kQueue &&
      qos.history.depth() > kShmRingCapacity) {
    // The per-subscriber ring reservation bounds queue depth at 16 on this
    // backend — refused, never silently clamped (documented divergence).
    return impl;  // kUnsupportedReach
  }
  std::unique_ptr<ShmTopicAttachment<T>> attachment;
  switch (ShmTopicAttachment<T>::Attach(ShmSegmentName(key, topic),
                                        SchemaHashOf<T>(), qos, &attachment)) {
    case ShmAttachStatus::kOk:
      break;
    case ShmAttachStatus::kTypeMismatch:
      impl->subscribe_status_ = SubscribeStatus::kTypeMismatch;  // R6
      return impl;
    case ShmAttachStatus::kUnavailable:
      return impl;  // kUnsupportedReach
  }
  impl->attachment_ = std::move(attachment);
  if (impl->ClaimSubscriberSlot()) {
    impl->subscribe_status_ = SubscribeStatus::kOk;
  } else {
    assert(false &&
           "xmMessaging: kShmMaxSubscribers exceeded on one shm topic");
    impl->attachment_.reset();
    // Release build: explicit refusal, not a silently-unconnected endpoint.
  }
  return impl;
}

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

#endif  // XMMESSAGING_HAS_POSIX_SHM

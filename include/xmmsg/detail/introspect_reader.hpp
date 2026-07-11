/*
 * detail/introspect_reader.hpp
 *
 * The R5 external-introspection reader for the POSIX-shm backend (P1b
 * introspection follow-up): everything an outside observer needs to
 * enumerate and diagnose a domain's topics with ZERO cooperation from the
 * observed processes (M10). There is no separate introspection segment —
 * the per-topic transport segment header (shm_segment.hpp) IS the
 * introspection surface, by design (design.md R5: transport and
 * introspection share the shm substrate). This reader adds the two things
 * the header alone does not give an outsider:
 *
 *   1. DISCOVERY — segments are named objects in /dev/shm following the
 *      wire-contract §6.4 grammar ("xmmsg.<isolation-key>.<topic>"), so a
 *      directory scan plus the documented name grammar IS the discovery
 *      mechanism (daemonless: no registry process to ask). Every candidate
 *      is validated by magic + layout version before anything else is
 *      trusted; a foreign file that happens to match the glob is reported
 *      as kForeign and skipped, never crashed on.
 *
 *   2. READ-ONLY ATTACHMENT — the observer opens O_RDONLY and maps
 *      PROT_READ, so it is PHYSICALLY unable to perturb transport state
 *      (M10-A4: the kernel enforces observer invisibility, not politeness).
 *      Every field an observer needs — identity, QoS record, liveness,
 *      counters, the master slot's last-publish envelope — is readable
 *      through atomic loads and a bounded seqlock read; nothing in the
 *      introspection protocol requires a write. (The library's OWN wiring
 *      paths do write — CAS claims, refusal records — but those are
 *      participants, not observers.)
 *
 * Read protocol (wire-contract §8, normative there): validate magic /
 * layout_version / init_state / total_size in that order; header scalars
 * are init-once (published by init_state's release store); mutable fields
 * are single-writer atomics loaded relaxed; the master slot's envelope is
 * read via the same writer-progress-only seqlock the transport uses, with
 * a bounded retry budget so a writer SIGKILLed mid-store yields kStalled
 * instead of a hang (M10-A5). The observer never blocks, never takes a
 * lock, and never observes torn bytes.
 *
 * detail/: not part of the portable API surface. Consumers today: the
 * `xmmsg` CLI (tools/xmmsg/) and the M10/M11 behavioral tests. Linux-only,
 * gated on XMMESSAGING_HAS_POSIX_SHM like the backend it observes.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xmmsg/detail/latest_slot.hpp"  // CpuRelax (seqlock retries)
#include "xmmsg/detail/shm_segment.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// Seqlock retry budget for the observer's master-slot read. Matches the
// transport's own budget rationale (posix_shm.hpp): thousands of
// consecutive overwrites during one read is not a live-writer profile, so
// exhausting it means "writer died mid-store" (or pathological pressure);
// either way the observer reports kStalled and moves on — never spins
// forever (M10-A5).
inline constexpr std::uint32_t kIntrospectRetryBudget = 4096;

// ---- discovery --------------------------------------------------------------

// One /dev/shm entry matching the §6.4 name grammar. Parsed from the NAME
// only — nothing here is validated against the segment contents yet.
struct DiscoveredSegment {
  std::string name;           // shm object name, with the leading '/'
  std::string isolation_key;  // "u<euid>.<domain>" (§6.2); empty if hashed
  std::string topic;          // dotted topic; empty if hashed
  bool hashed_name = false;   // the §6.4 over-long fallback "xmmsg.h<16hex>"
};

// Parse "xmmsg.<key>.<topic>" per §6.4/§6.2: the isolation key is exactly
// two dot-separated segments ("u<euid>" + sanitized domain name — the
// domain sanitizer never emits '.'), the topic is everything after them.
// Returns false for names that carry the prefix but not the grammar.
inline bool ParseSegmentName(const std::string& shm_name,
                             DiscoveredSegment* out) {
  constexpr const char kPrefix[] = "/xmmsg.";
  constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (shm_name.compare(0, kPrefixLen, kPrefix) != 0) {
    return false;
  }
  out->name = shm_name;
  out->isolation_key.clear();
  out->topic.clear();
  out->hashed_name = false;
  const std::string rest = shm_name.substr(kPrefixLen);
  // Hashed fallback form: "h" + exactly 16 lowercase hex digits (§6.4).
  if (rest.size() == 17 && rest[0] == 'h') {
    bool hex = true;
    for (std::size_t i = 1; i < rest.size(); ++i) {
      const char c = rest[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        hex = false;
        break;
      }
    }
    if (hex) {
      out->hashed_name = true;
      return true;
    }
  }
  const std::size_t dot1 = rest.find('.');
  if (dot1 == std::string::npos) {
    return false;
  }
  const std::size_t dot2 = rest.find('.', dot1 + 1);
  if (dot2 == std::string::npos || dot2 + 1 >= rest.size()) {
    return false;  // need key (2 segments) + a non-empty topic
  }
  if (rest[0] != 'u') {
    return false;  // §6.2: keys start "u<euid>"
  }
  out->isolation_key = rest.substr(0, dot2);
  out->topic = rest.substr(dot2 + 1);
  return true;
}

// Scan /dev/shm for xmmsg-named objects. Name-grammar filter only; callers
// validate each candidate by opening it (magic/version) before trusting it.
inline std::vector<DiscoveredSegment> DiscoverXmmsgSegments() {
  std::vector<DiscoveredSegment> found;
  DIR* dir = ::opendir("/dev/shm");
  if (dir == nullptr) {
    return found;
  }
  while (const dirent* entry = ::readdir(dir)) {
    const std::string shm_name = std::string("/") + entry->d_name;
    DiscoveredSegment segment;
    if (ParseSegmentName(shm_name, &segment)) {
      found.push_back(std::move(segment));
    }
  }
  ::closedir(dir);
  return found;
}

// ---- snapshot ---------------------------------------------------------------

struct IntrospectSubSlot {
  std::uint32_t index = 0;
  std::uint32_t state = kShmSubFree;  // kShmSub{Free,Claimed,Active}
  std::uint32_t pid = 0;
  bool alive = false;  // kill(pid,0) probe at snapshot time
  std::uint32_t history_kind = 0;  // 0 latest-only, 1 queue
  std::uint32_t queue_depth = 0;
  std::uint64_t last_consumed_ordinal = 0;
  std::uint64_t take_count = 0;
  std::uint64_t drop_count = 0;
  std::uint64_t overwrite_count = 0;
  std::uint64_t deadline_miss_count = 0;
};

// How the master slot's last-publish read ended (§8 read protocol).
enum class MasterReadResult : std::uint8_t {
  kEmpty,    // never written (or crash-repaired to empty)
  kValue,    // envelope fields below are a validated, untorn copy
  kStalled,  // retry budget exhausted: writer dead mid-store or extreme
             // overwrite pressure — fields below are NOT valid
};

struct IntrospectSnapshot {
  // Identity (init-once header fields).
  std::uint32_t layout_version = 0;
  std::uint32_t envelope_version = 0;
  std::uint64_t schema_hash = 0;
  std::uint64_t payload_size = 0;
  std::uint64_t payload_align = 0;
  std::uint64_t total_size = 0;
  std::uint32_t max_subscribers = 0;
  std::uint32_t ring_capacity = 0;
  std::uint32_t creator_history_kind = 0;  // 0 latest-only, 1 queue
  std::uint32_t creator_queue_depth = 0;

  // Publisher liveness + cumulative counters.
  std::uint32_t pub_pid = 0;  // 0 = no publisher has the slot
  bool pub_alive = false;
  std::uint32_t pub_epoch = 0;  // publisher generations (M4-A4)
  std::uint64_t pub_publish_count = 0;
  std::uint64_t pub_bytes = 0;
  std::uint64_t accepted_ordinal = 0;

  // R6 refusal-visibility record (M11-A3; zero refusal_count = never).
  std::uint64_t refusal_count = 0;
  std::uint64_t refused_schema_hash = 0;
  std::uint64_t refused_payload_size = 0;
  std::uint32_t refused_pid = 0;

  // Master-slot last publish (bounded seqlock read; §8).
  MasterReadResult master = MasterReadResult::kEmpty;
  std::int64_t last_publish_stamp_ns = 0;  // CLOCK_MONOTONIC (R8, same host)
  std::int64_t last_origin_stamp_ns = 0;
  std::uint64_t last_ordinal = 0;

  std::vector<IntrospectSubSlot> subscribers;  // active slots only
  std::uint32_t active_subscriber_count = 0;
};

enum class IntrospectOpenStatus : std::uint8_t {
  kOk,
  kForeign,      // exists but is not (or no longer parses as) an xmmsg
                 // segment — wrong magic or inconsistent sizes; skip it
  kVersionSkew,  // xmmsg magic but a layout version this reader does not
                 // implement — refuse-unknown (§8), never reinterpret
  kNotReady,     // creator has not published init_state yet (mid-wiring)
  kUnavailable,  // OS refusal (ENOENT raced, EACCES, mmap failure)
};

// A read-only mapping of one topic segment. Move-only; unmaps on
// destruction. All methods are observer-safe: no locks, no writes, bounded
// retries, wiring-time syscalls only (open/fstat/mmap in Open, kill(pid,0)
// probes in Snapshot — the observed processes' hot path is untouched).
class IntrospectReader {
 public:
  IntrospectReader() = default;
  IntrospectReader(const IntrospectReader&) = delete;
  IntrospectReader& operator=(const IntrospectReader&) = delete;
  IntrospectReader(IntrospectReader&& other) noexcept
      : base_(other.base_), size_(other.size_), name_(std::move(other.name_)) {
    other.base_ = nullptr;
    other.size_ = 0;
  }
  IntrospectReader& operator=(IntrospectReader&& other) noexcept {
    if (this != &other) {
      Unmap();
      base_ = other.base_;
      size_ = other.size_;
      name_ = std::move(other.name_);
      other.base_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }
  ~IntrospectReader() { Unmap(); }

  const std::string& name() const noexcept { return name_; }

  // O_RDONLY + PROT_READ: the observer cannot write, by construction.
  // Validation order per §8: size sanity, init barrier, magic,
  // layout/envelope version, declared-vs-actual size.
  static IntrospectOpenStatus Open(const std::string& name,
                                   IntrospectReader* out) {
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
      return IntrospectOpenStatus::kUnavailable;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return IntrospectOpenStatus::kUnavailable;
    }
    if (static_cast<std::size_t>(st.st_size) < sizeof(ShmSegmentHeader)) {
      ::close(fd);
      // Too small to even hold a header: a creator mid-ftruncate (not ready)
      // or a foreign file; either way there is nothing to read yet.
      return IntrospectOpenStatus::kNotReady;
    }
    const std::size_t map_size = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
      return IntrospectOpenStatus::kUnavailable;
    }
    const auto* h = static_cast<const ShmSegmentHeader*>(map);
    // The init barrier first: before init_state's release store the
    // identity fields are unwritten zeros, not lies to be validated.
    if (h->init_state.load(std::memory_order_acquire) != 1) {
      ::munmap(map, map_size);
      return IntrospectOpenStatus::kNotReady;
    }
    if (h->magic != kShmMagic) {
      ::munmap(map, map_size);
      return IntrospectOpenStatus::kForeign;
    }
    if (h->layout_version != kShmLayoutVersion ||
        h->envelope_version != kShmEnvelopeVersion) {
      ::munmap(map, map_size);
      return IntrospectOpenStatus::kVersionSkew;
    }
    if (h->total_size != static_cast<std::uint64_t>(st.st_size) ||
        h->max_subscribers != kShmMaxSubscribers) {
      // Declared size disagrees with the object: refuse rather than walk
      // off the mapping (a foreign or corrupted object).
      ::munmap(map, map_size);
      return IntrospectOpenStatus::kForeign;
    }
    out->Unmap();
    out->base_ = static_cast<const unsigned char*>(map);
    out->size_ = map_size;
    out->name_ = name;
    return IntrospectOpenStatus::kOk;
  }

  // One coherent-enough picture of the topic: identity fields are
  // init-once; counters are independent single-writer atomics (each value
  // exact, the set not mutually snapshotted — the documented monotonic-
  // counter contract); the master envelope is a seqlock-validated copy.
  // Never blocks: worst case is the bounded retry budget (M10-A5).
  bool Snapshot(IntrospectSnapshot* out) const {
    if (base_ == nullptr) {
      return false;
    }
    const auto* h = reinterpret_cast<const ShmSegmentHeader*>(base_);
    out->layout_version = h->layout_version;
    out->envelope_version = h->envelope_version;
    out->schema_hash = h->schema_hash;
    out->payload_size = h->payload_size;
    out->payload_align = h->payload_align;
    out->total_size = h->total_size;
    out->max_subscribers = h->max_subscribers;
    out->ring_capacity = h->ring_capacity;
    out->creator_history_kind = h->creator_history_kind;
    out->creator_queue_depth = h->creator_queue_depth;

    out->pub_pid = h->pub_pid.load(std::memory_order_acquire);
    out->pub_alive = out->pub_pid != 0 && ProcessAlive(out->pub_pid);
    out->pub_epoch = h->pub_epoch.load(std::memory_order_relaxed);
    out->pub_publish_count =
        h->pub_publish_count.load(std::memory_order_relaxed);
    out->pub_bytes = h->pub_bytes.load(std::memory_order_relaxed);
    out->accepted_ordinal =
        h->accepted_ordinal.load(std::memory_order_relaxed);

    out->refusal_count = h->refusal_count.load(std::memory_order_acquire);
    out->refused_schema_hash =
        h->refused_schema_hash.load(std::memory_order_relaxed);
    out->refused_payload_size =
        h->refused_payload_size.load(std::memory_order_relaxed);
    out->refused_pid = h->refused_pid.load(std::memory_order_relaxed);

    out->subscribers.clear();
    out->active_subscriber_count = 0;
    for (std::uint32_t i = 0; i < kShmMaxSubscribers; ++i) {
      const ShmSubSlot& slot = h->sub_slots[i];
      const std::uint32_t state = slot.state.load(std::memory_order_acquire);
      if (state == kShmSubFree) {
        continue;
      }
      IntrospectSubSlot sub;
      sub.index = i;
      sub.state = state;
      sub.pid = slot.pid.load(std::memory_order_relaxed);
      sub.alive = sub.pid != 0 && ProcessAlive(sub.pid);
      sub.history_kind = slot.history_kind.load(std::memory_order_relaxed);
      sub.queue_depth = slot.queue_depth.load(std::memory_order_relaxed);
      sub.last_consumed_ordinal =
          slot.last_consumed_ordinal.load(std::memory_order_relaxed);
      sub.take_count = slot.take_count.load(std::memory_order_relaxed);
      sub.drop_count = slot.drop_count.load(std::memory_order_relaxed);
      sub.overwrite_count =
          slot.overwrite_count.load(std::memory_order_relaxed);
      sub.deadline_miss_count =
          slot.deadline_miss_count.load(std::memory_order_relaxed);
      if (state == kShmSubActive) {
        ++out->active_subscriber_count;
      }
      out->subscribers.push_back(sub);
    }

    ReadMasterEnvelope(out);
    return true;
  }

 private:
  // Type-erased bounded seqlock read of the master slot's ENVELOPE words.
  // The observer does not know T, but it does not need to: the record
  // layout ahead of the payload is fixed (48-byte envelope + u64 ordinal,
  // wire-contract §6.4), the cell sits at the layout-computed 64-aligned
  // offset after the header, and reading a PREFIX of the record's words
  // under the seqlock is exactly as torn-proof as reading all of them —
  // validation is on the sequence, not the byte count. Same memory-
  // ordering pairs as LatestSlot::LoadBounded (latest_slot.hpp R1/R2).
  static constexpr std::size_t kMasterOffset =
      (sizeof(ShmSegmentHeader) + 63u) & ~std::size_t{63u};
  // Words 0..5 are the 48-byte envelope; word 6 is the transport ordinal.
  static constexpr std::size_t kEnvelopeWords = 7;
  static constexpr std::size_t kPublishStampWord = 3;  // record byte 24
  static constexpr std::size_t kOriginStampWord = 4;   // record byte 32
  static constexpr std::size_t kOrdinalWord = 6;       // record byte 48

  void ReadMasterEnvelope(IntrospectSnapshot* out) const {
    out->master = MasterReadResult::kStalled;
    out->last_publish_stamp_ns = 0;
    out->last_origin_stamp_ns = 0;
    out->last_ordinal = 0;
    // Guard the prefix read against a lying total_size (already validated
    // against the real object size in Open).
    if (kMasterOffset + (1 + kEnvelopeWords) * sizeof(std::uint64_t) >
        size_) {
      out->master = MasterReadResult::kEmpty;
      return;
    }
    const auto* seq = reinterpret_cast<const std::atomic<std::uint64_t>*>(
        base_ + kMasterOffset);
    const auto* words = seq + 1;
    std::uint64_t staged[kEnvelopeWords];
    for (std::uint32_t attempt = 0; attempt <= kIntrospectRetryBudget;
         ++attempt) {
      const std::uint64_t s1 = seq->load(std::memory_order_acquire);  // (R1)
      if (s1 == 0) {
        out->master = MasterReadResult::kEmpty;
        return;
      }
      if ((s1 & 1u) != 0u) {  // write in progress — or writer died mid-store
        CpuRelax();
        continue;
      }
      for (std::size_t i = 0; i < kEnvelopeWords; ++i) {
        staged[i] = words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);  // (R2)
      if (seq->load(std::memory_order_relaxed) == s1) {
        out->master = MasterReadResult::kValue;
        out->last_publish_stamp_ns =
            static_cast<std::int64_t>(staged[kPublishStampWord]);
        out->last_origin_stamp_ns =
            static_cast<std::int64_t>(staged[kOriginStampWord]);
        out->last_ordinal = staged[kOrdinalWord];
        return;
      }
      CpuRelax();
    }
    // Budget exhausted: kStalled (set above) — the honest answer when the
    // writer died mid-store; the caller reports "last publish unknown".
  }

  void Unmap() noexcept {
    if (base_ != nullptr) {
      ::munmap(const_cast<unsigned char*>(base_), size_);
      base_ = nullptr;
      size_ = 0;
    }
  }

  const unsigned char* base_ = nullptr;
  std::size_t size_ = 0;
  std::string name_;
};

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

#endif  // XMMESSAGING_HAS_POSIX_SHM

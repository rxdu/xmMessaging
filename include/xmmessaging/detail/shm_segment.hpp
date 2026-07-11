/*
 * detail/shm_segment.hpp
 *
 * The POSIX shm fallback backend's substrate (P1b): one named shared-memory
 * segment per topic, holding a fixed header (identity, QoS bounds, liveness,
 * counters) followed by the data plane (master latest slot + per-subscriber
 * mailboxes/rings, laid out by posix_shm.hpp).
 *
 * shm_open vs memfd_create — DECIDED: shm_open. memfd_create gives sealing
 * (F_SEAL_SHRINK etc.) but a memfd is anonymous: a peer can only obtain it
 * by fd inheritance or SCM_RIGHTS passing, i.e. it presumes a common
 * ancestor or a broker socket. Both violate this backend's contracts:
 * INDEPENDENT processes must rendezvous by NAME in arbitrary start order
 * (M14-A1, "wiring is order-independent"), and the design is daemonless
 * (M4) — there is no broker to pass fds. shm_open gives exactly the named,
 * ancestor-free rendezvous the contracts require; what sealing would have
 * protected against (a peer resizing the object) is covered by validating
 * the header's total_size against the locally-computed layout before
 * touching the data plane. The creation race (two processes advertising/
 * subscribing the same topic simultaneously) is settled by O_CREAT|O_EXCL:
 * exactly one creator wins, everyone else attaches and validates.
 *
 * Segment lifecycle — DECIDED: named segments are NEVER unlinked by the
 * library. "Last detacher unlinks" is racy (a new attacher between the
 * last detach and the unlink attaches a doomed name), and unlinking on
 * publisher exit would destroy the M2/M4 warm-start value that is the
 * point of keeping the slot in shared memory. Segments are small, bounded
 * in number by the application's topic set, namespaced by the isolation
 * key (wire-contract §6.2), and live in /dev/shm (tmpfs: pages are
 * allocated on first touch, so reserved-but-unused ring capacity costs
 * address space, not RAM). Cleanup is an explicit operation: the future
 * `xmmsg` CLI (R5, P1b introspection follow-up) gets a `clean <domain>`
 * verb; until then `rm /dev/shm/xmmsg.<key>.*` is the documented manual
 * path. Tests clean up after themselves via UnlinkSegment below.
 *
 * Crash safety (M4): the kernel owns the pages — a SIGKILLed participant
 * releases nothing and corrupts no lock, because there are no locks to
 * hold. A writer killed mid-store leaves a seqlock cell with an odd
 * sequence, which readers detect and skip (LatestSlot::LoadBounded) and
 * the next claiming writer repairs (RepairAfterWriterCrash) — the
 * "skippable sequence, no robust-lock recovery" story from design.md.
 *
 * detail/: not part of the portable API surface. Linux-only by charter
 * (design.md: this IS the Linux fallback); the whole backend is gated on
 * XMMESSAGING_HAS_POSIX_SHM.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include "xmmessaging/detail/schema_hash.hpp"
#include "xmmessaging/qos.hpp"

namespace xmotion {
namespace messaging {
namespace detail {

// ---- segment constants (part of the layout contract; bumping any of them
// bumps kShmLayoutVersion so mismatched builds refuse instead of corrupt) --
inline constexpr std::uint64_t kShmMagic = 0x31455347534D4D58ULL;  // "XMMSGSE1"
// v2 (P1b introspection follow-up): the header gained the R6 refusal-
// visibility record (refusal_count / refused_* below, M11-A3). v1 readers
// would compute wrong data-plane offsets, so the version bump makes them
// refuse instead (the refuse-unknown rule).
inline constexpr std::uint32_t kShmLayoutVersion = 2;
inline constexpr std::uint32_t kShmEnvelopeVersion = 0;  // wire-contract §2
// Per-topic subscriber bound (the in-process reach bounds at 64; the shm
// segment reserves real ring capacity per slot, so the bound is tighter —
// a wiring-time fact, documented in the spec's shm section).
inline constexpr std::uint32_t kShmMaxSubscribers = 16;
// Per-subscriber ring reservation, in records. A queue<N> subscriber with
// N <= this bound gets its declared depth exactly; N beyond it is refused
// at wiring (a stated, queryable divergence — never a silent clamp).
// tmpfs allocates pages on first touch, so unused reservation is free.
inline constexpr std::uint32_t kShmRingCapacity = 16;

// Subscriber slot states.
inline constexpr std::uint32_t kShmSubFree = 0;
inline constexpr std::uint32_t kShmSubClaimed = 1;  // mid-(re)initialization
inline constexpr std::uint32_t kShmSubActive = 2;

// D6 baseline sentinel (same value and same accounting contract as the
// in-process SubCore — the mechanism moved into shared memory unchanged).
inline constexpr std::uint64_t kShmBaselineUnset = UINT64_MAX;

// ---- per-subscriber slot (header-resident; the mailbox/ring data lives in
// the data plane at a layout-computed offset) -------------------------------
struct ShmSubSlot {
  std::atomic<std::uint32_t> state;         // kShmSub{Free,Claimed,Active}
  std::atomic<std::uint32_t> pid;           // owner, for liveness (M4-A4)
  std::atomic<std::uint32_t> history_kind;  // 0 latest-only, 1 queue
  std::atomic<std::uint32_t> queue_depth;   // declared depth (queue kind)
  // D6/D9 accounting — shared atomics so the publisher can stamp the join
  // baseline and count per-subscriber drops cross-process, and so
  // introspection reads exact counters without the subscriber's help.
  std::atomic<std::uint64_t> last_consumed_ordinal;
  std::atomic<std::uint64_t> take_count;
  std::atomic<std::uint64_t> drop_count;
  std::atomic<std::uint64_t> overwrite_count;
  std::atomic<std::uint64_t> deadline_miss_count;
};

// ---- segment header --------------------------------------------------------
// Identity fields are plain (written once by the creator, read-only after
// init_state publishes them); everything mutable is an atomic. Designed to
// be EXTENDED by the P1b introspection work (R5): the header already carries
// the identity, liveness, and counter surface an external observer needs;
// the introspection segment shares this substrate (design.md).
struct ShmSegmentHeader {
  std::uint64_t magic;             // kShmMagic
  std::uint32_t layout_version;    // kShmLayoutVersion — refuse-unknown
  std::uint32_t envelope_version;  // wire-contract §2 version carried inside
  std::uint64_t schema_hash;       // R6 gate (M14-A1: order-independent)
  std::uint64_t payload_size;      // consistency check vs local type
  std::uint64_t payload_align;
  std::uint64_t total_size;        // full segment size — validated by every
                                   // attacher before the data plane is used
  std::uint32_t max_subscribers;   // kShmMaxSubscribers at creation
  std::uint32_t ring_capacity;     // kShmRingCapacity at creation
  // Creator's declared QoS, recorded for introspection (matching does not
  // gate on it: history is a per-subscriber declaration, D7).
  std::uint32_t creator_history_kind;
  std::uint32_t creator_queue_depth;

  // Creation barrier: 0 while the creator initializes, 1 (release) when the
  // data plane is ready. Attachers acquire-poll it (bounded).
  std::atomic<std::uint32_t> init_state;
  // Cross-process futex word (waiter.hpp FutexWaiter shm form). No P1b verb
  // parks on it yet — pub/sub take verbs never block (D5) and readiness
  // polls — but the wake seam is segment-resident from day one so a future
  // event-driven verb needs no layout bump.
  std::atomic<std::uint32_t> futex_word;

  // Topic ordinal author (mail_record.hpp). Lives HERE, not in a process,
  // so a restarted publisher resumes the ordinal sequence exactly where the
  // dead one stopped (M4-A2): ordinals stay contiguous over accepted
  // publishes across crashes, keeping D6/D9 gap accounting exact.
  std::atomic<std::uint64_t> accepted_ordinal;

  // Publisher liveness slot (D15 exclusive ownership, M4). pid == 0 means
  // "no publisher". Claim protocol: CAS 0 -> pid; on failure, kill(pid, 0)
  // == ESRCH proves the owner dead and the slot reclaimable (wiring path
  // only — never on the hot path). pub_epoch counts publisher generations
  // (a restart is observable, M4-A4).
  std::atomic<std::uint32_t> pub_pid;
  std::atomic<std::uint32_t> pub_epoch;
  // Cumulative publish counters (observability; endpoint-lifetime counters
  // for D9 stay process-local on the endpoint, matching in-process
  // semantics).
  std::atomic<std::uint64_t> pub_publish_count;
  std::atomic<std::uint64_t> pub_bytes;

  // R6 refusal-visibility record (M11-A3, layout v2): the most recent
  // type-mismatch refusal on this topic, written by the REFUSED attacher on
  // its wiring path (OpenOrCreate's kTypeMismatch exit) so an external
  // observer can show BOTH hashes — the topic's established schema_hash
  // above and the hash the refused endpoint arrived with. Deliberately a
  // last-writer-wins advisory slot, the one stated exception to the
  // single-writer-per-slot rule (wire-contract §8): every field is one
  // atomic (no torn reads possible), writers are wiring-path only (never
  // the data path), and concurrent refusals can interleave fields — the
  // monotonic refusal_count is the reliable "a refusal happened" signal,
  // the refused_* fields identify the latest offender.
  std::atomic<std::uint64_t> refusal_count;        // 0 = never refused
  std::atomic<std::uint64_t> refused_schema_hash;  // §4.1 rendering: 0x%016X
  std::atomic<std::uint64_t> refused_payload_size;
  std::atomic<std::uint32_t> refused_pid;
  std::uint32_t reserved0;  // explicit padding (deterministic header bytes)

  ShmSubSlot sub_slots[kShmMaxSubscribers];
};

static_assert(std::is_trivially_destructible_v<ShmSegmentHeader>,
              "the mapping owns the header; it is never destroyed");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
                  std::atomic<std::uint32_t>::is_always_lock_free,
              "segment atomics must be address-free (lock-free) to be "
              "shared across mappings");
// The header byte layout is published (wire-contract §8, layout v2) so an
// external reader can be written from the spec alone. These asserts keep
// the struct honest to the published offsets; any drift is a compile error
// AND a kShmLayoutVersion bump.
static_assert(std::is_standard_layout_v<ShmSegmentHeader>,
              "the header layout is a cross-process contract");
static_assert(sizeof(ShmSubSlot) == 56, "published sub-slot stride (§8)");
static_assert(offsetof(ShmSegmentHeader, schema_hash) == 16 &&
                  offsetof(ShmSegmentHeader, init_state) == 64 &&
                  offsetof(ShmSegmentHeader, accepted_ordinal) == 72 &&
                  offsetof(ShmSegmentHeader, pub_pid) == 80 &&
                  offsetof(ShmSegmentHeader, pub_publish_count) == 88 &&
                  offsetof(ShmSegmentHeader, refusal_count) == 104 &&
                  offsetof(ShmSegmentHeader, sub_slots) == 136 &&
                  sizeof(ShmSegmentHeader) == 1032,
              "published header offsets (wire-contract §8, layout v2)");

// ---- naming (wire-contract §6.2, resolved at P1b) --------------------------
// Segment name: "/xmmsg.<isolation-key>.<topic>". The isolation key is
// "u<euid>.<sanitized-domain-name>" (domain.cpp DeriveIsolationKey); the
// sanitizer below maps anything outside [a-z0-9_.] (uppercase folded) to
// '_'. shm_open names allow one leading '/' and no others; NAME_MAX bounds
// the whole name, so an over-long name degrades to the documented hashed
// form "xmmsg.h<16 hex digits>" — unreadable but collision-safe, and
// reproducible from the spec.
inline std::string SanitizeShmNamePart(std::string_view part) {
  std::string out;
  out.reserve(part.size());
  for (char c : part) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '.') {
      out += c;
    } else if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += '_';
    }
  }
  return out;
}

inline std::string ShmSegmentName(const std::string& isolation_key,
                                  std::string_view topic) {
  std::string name = "/xmmsg." + SanitizeShmNamePart(isolation_key) + "." +
                     SanitizeShmNamePart(topic);
  constexpr std::size_t kNameBound = 240;  // < NAME_MAX with margin
  if (name.size() > kNameBound) {
    char hex[17];
    const std::uint64_t hash = Fnv1a64(name);
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    name = std::string("/xmmsg.h") + hex;
  }
  return name;
}

// Explicit cleanup (tests, future CLI). Never called on detach — see the
// lifecycle decision in the header comment.
inline void UnlinkSegment(const std::string& name) {
  ::shm_unlink(name.c_str());
}

// Liveness probe (wiring/readiness paths only, never the hot path). EPERM
// means "exists but not ours" — alive. Known limit, documented: a recycled
// pid reads as alive; the consequence is a refused claim, never corruption.
inline bool ProcessAlive(std::uint32_t pid) {
  if (pid == 0) {
    return false;
  }
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

// ---- mapping ---------------------------------------------------------------
enum class ShmAttachStatus : std::uint8_t {
  kOk,
  kTypeMismatch,  // R6: existing segment carries a different schema hash
  kUnavailable,   // OS refusal / half-initialized orphan / size mismatch
};

class ShmMapping {
 public:
  ShmMapping() = default;
  ShmMapping(const ShmMapping&) = delete;
  ShmMapping& operator=(const ShmMapping&) = delete;
  ShmMapping(ShmMapping&& other) noexcept
      : base_(other.base_), size_(other.size_), created_(other.created_) {
    other.base_ = nullptr;
    other.size_ = 0;
  }
  ShmMapping& operator=(ShmMapping&& other) noexcept {
    if (this != &other) {
      Unmap();
      base_ = other.base_;
      size_ = other.size_;
      created_ = other.created_;
      other.base_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }
  ~ShmMapping() { Unmap(); }

  unsigned char* base() const noexcept { return base_; }
  std::size_t size() const noexcept { return size_; }
  bool created() const noexcept { return created_; }
  ShmSegmentHeader* header() const noexcept {
    return reinterpret_cast<ShmSegmentHeader*>(base_);
  }

  // Open-or-create + validate. On kOk the mapping covers the whole segment;
  // if created() the caller MUST initialize the data plane and then call
  // MarkReady() — until then attachers wait on init_state.
  //
  // All syscalls here are wiring-time (Advertise/Subscribe); the hot path
  // never re-enters this file (R7).
  static ShmAttachStatus OpenOrCreate(const std::string& name,
                                      std::uint64_t schema_hash,
                                      std::uint64_t payload_size,
                                      std::uint64_t payload_align,
                                      std::size_t total_size, const Qos& qos,
                                      ShmMapping* out);

  void MarkReady() noexcept {
    header()->init_state.store(1, std::memory_order_release);
  }

 private:
  void Unmap() noexcept {
    if (base_ != nullptr) {
      ::munmap(base_, size_);
      base_ = nullptr;
    }
  }

  unsigned char* base_ = nullptr;
  std::size_t size_ = 0;
  bool created_ = false;
};

inline ShmAttachStatus ShmMapping::OpenOrCreate(
    const std::string& name, std::uint64_t schema_hash,
    std::uint64_t payload_size, std::uint64_t payload_align,
    std::size_t total_size, const Qos& qos, ShmMapping* out) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    // Creator path: O_CREAT|O_EXCL makes creation race-safe — exactly one
    // winner per name, however many processes wire simultaneously (M14-A1).
    int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
      if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
        ::close(fd);
        ::shm_unlink(name.c_str());  // never existed for anyone else
        return ShmAttachStatus::kUnavailable;
      }
      void* map = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
      ::close(fd);  // the mapping keeps the object alive
      if (map == MAP_FAILED) {
        ::shm_unlink(name.c_str());
        return ShmAttachStatus::kUnavailable;
      }
      out->base_ = static_cast<unsigned char*>(map);
      out->size_ = total_size;
      out->created_ = true;
      auto* h = out->header();
      // tmpfs pages arrive zeroed; init_state is therefore already 0 (raw)
      // for every attacher that raced in between ftruncate and here.
      h->magic = kShmMagic;
      h->layout_version = kShmLayoutVersion;
      h->envelope_version = kShmEnvelopeVersion;
      h->schema_hash = schema_hash;
      h->payload_size = payload_size;
      h->payload_align = payload_align;
      h->total_size = total_size;
      h->max_subscribers = kShmMaxSubscribers;
      h->ring_capacity = kShmRingCapacity;
      h->creator_history_kind =
          qos.history.kind() == History::Kind::kLatestOnly ? 0u : 1u;
      h->creator_queue_depth = qos.history.depth();
      return ShmAttachStatus::kOk;  // caller initializes + MarkReady()
    }
    if (errno != EEXIST) {
      return ShmAttachStatus::kUnavailable;
    }

    // Attacher path.
    fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
      if (errno == ENOENT) {
        continue;  // creator vanished between EEXIST and open — retry
      }
      return ShmAttachStatus::kUnavailable;
    }
    // Bounded wait for the creator: size grown to at least the header, then
    // init_state == ready. A creator that died mid-init leaves an orphan we
    // refuse (kUnavailable) rather than adopt — rare, documented; manual/
    // CLI cleanup applies. 2 s bound at 1 ms polls (wiring path).
    constexpr int kPolls = 2000;
    struct stat st {};
    int poll = 0;
    for (; poll < kPolls; ++poll) {
      if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return ShmAttachStatus::kUnavailable;
      }
      if (static_cast<std::size_t>(st.st_size) >= sizeof(ShmSegmentHeader)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (poll == kPolls) {
      ::close(fd);
      return ShmAttachStatus::kUnavailable;
    }
    void* head_map = ::mmap(nullptr, sizeof(ShmSegmentHeader),
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (head_map == MAP_FAILED) {
      ::close(fd);
      return ShmAttachStatus::kUnavailable;
    }
    auto* h = static_cast<ShmSegmentHeader*>(head_map);
    bool ready = false;
    for (poll = 0; poll < kPolls; ++poll) {
      if (h->init_state.load(std::memory_order_acquire) == 1) {
        ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ready) {
      ::munmap(head_map, sizeof(ShmSegmentHeader));
      ::close(fd);
      return ShmAttachStatus::kUnavailable;
    }
    // Identity gates, in refuse-don't-reinterpret order (R6, M14-A1: the
    // segment header makes matching order-independent — whoever arrives
    // second validates against whoever arrived first).
    if (h->magic != kShmMagic || h->layout_version != kShmLayoutVersion ||
        h->envelope_version != kShmEnvelopeVersion) {
      ::munmap(head_map, sizeof(ShmSegmentHeader));
      ::close(fd);
      return ShmAttachStatus::kUnavailable;
    }
    if (h->schema_hash != schema_hash || h->payload_size != payload_size ||
        h->payload_align != payload_align) {
      // R6 refusal, made VISIBLE (M11-A3): before walking away, the refused
      // attacher records what it arrived with, so external introspection can
      // show both hashes. Wiring path, advisory last-writer-wins slot — see
      // the field comment in ShmSegmentHeader.
      h->refused_schema_hash.store(schema_hash, std::memory_order_relaxed);
      h->refused_payload_size.store(payload_size, std::memory_order_relaxed);
      h->refused_pid.store(static_cast<std::uint32_t>(::getpid()),
                           std::memory_order_relaxed);
      h->refusal_count.fetch_add(1, std::memory_order_release);
      ::munmap(head_map, sizeof(ShmSegmentHeader));
      ::close(fd);
      return ShmAttachStatus::kTypeMismatch;
    }
    if (h->total_size != total_size ||
        h->max_subscribers != kShmMaxSubscribers ||
        h->ring_capacity != kShmRingCapacity) {
      ::munmap(head_map, sizeof(ShmSegmentHeader));
      ::close(fd);
      return ShmAttachStatus::kUnavailable;
    }
    ::munmap(head_map, sizeof(ShmSegmentHeader));
    void* map =
        ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
      return ShmAttachStatus::kUnavailable;
    }
    out->base_ = static_cast<unsigned char*>(map);
    out->size_ = total_size;
    out->created_ = false;
    return ShmAttachStatus::kOk;
  }
  return ShmAttachStatus::kUnavailable;
}

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

#endif  // XMMESSAGING_HAS_POSIX_SHM

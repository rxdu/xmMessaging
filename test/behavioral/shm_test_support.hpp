/*
 * shm_test_support.hpp — shared fixture pieces for the POSIX-shm (P1b)
 * cross-process behavioral legs: the wire-described test payload, the
 * helper-binary spawner (fork + execv of shm_test_helper — cleaner under
 * gtest + sanitizers than forking the test binary itself: exec resets the
 * sanitizer runtime and the child runs a purpose-built single-threaded
 * main), and segment cleanup (the library deliberately never unlinks —
 * tests clean up after themselves).
 *
 * TSan caveat, stated per family rule: ThreadSanitizer instruments each
 * process separately and CANNOT see cross-process shm races — the TSan
 * runs of these tests verify the in-process halves (and that the fixture
 * itself is clean); the cross-process memory-ordering claims ride on the
 * seqlock/ring proofs from the in-process suite (same algorithms, same
 * orderings — the placement parameterization) plus the ASan leg, which
 * does catch mapping/bounds bugs.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#if defined(XMMESSAGING_HAS_POSIX_SHM)

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "xmmsg/messaging.hpp"

// ---------------------------------------------------------------------------
// The cross-process payload: fixed layout, explicitly padded (§3), opted
// into the canonical §4 schema hash — the form every boundary-crossing
// payload SHOULD use. checksum is written last / verified whole so a torn
// value is detectable (the M1-A2 technique, cross-process).
// ---------------------------------------------------------------------------
struct ShmTestPlan {
  std::uint64_t plan_id;  // marker, written first
  double x[8];
  double y[8];
  std::uint32_t checksum;  // written last
  std::uint32_t _pad0;     // explicit trailing padding (§3)
};
static_assert(sizeof(ShmTestPlan) == 144, "explicitly padded, fixed size");

inline std::uint32_t ShmPlanChecksum(std::uint64_t id) {
  std::uint64_t h = id * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 32;
  return static_cast<std::uint32_t>(h);
}

inline void FillShmPlan(ShmTestPlan& plan, std::uint64_t id) {
  plan.plan_id = id;
  for (int i = 0; i < 8; ++i) {
    plan.x[i] = static_cast<double>(id) + i;
    plan.y[i] = static_cast<double>(id) - i;
  }
  plan._pad0 = 0;
  plan.checksum = ShmPlanChecksum(id);
}

inline bool ShmPlanConsistent(const ShmTestPlan& plan) {
  if (plan.checksum != ShmPlanChecksum(plan.plan_id)) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if (plan.x[i] != static_cast<double>(plan.plan_id) + i ||
        plan.y[i] != static_cast<double>(plan.plan_id) - i) {
      return false;
    }
  }
  return true;
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

#if defined(XMMESSAGING_HAS_POSIX_SHM)
XMMSG_DESCRIBE(ShmTestPlan, XMMSG_FIELD(plan_id), XMMSG_FIELD(x),
               XMMSG_FIELD(y), XMMSG_FIELD(checksum), XMMSG_FIELD(_pad0));
#endif

#if defined(XMMESSAGING_HAS_POSIX_SHM) && !defined(XMMSG_SHM_HELPER_BUILD)

// ---------------------------------------------------------------------------
// Test-side process management.
// ---------------------------------------------------------------------------
namespace shmtest {

// Per-process-unique domain name: segments from this run never collide with
// another run's (the isolation key embeds the name — D17/§6.2).
inline std::string UniqueDomainName(const char* tag) {
  return std::string(tag) + "_" + std::to_string(::getpid());
}

// fork + execv of an arbitrary helper binary (M11 spawns per-variant
// builds; everything else uses the SpawnHelper wrapper below).
inline pid_t SpawnBinary(const std::string& binary,
                         const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const std::string& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(binary.c_str(), argv.data());
    ::_exit(127);  // exec failed
  }
  return pid;
}

// fork + execv of the helper binary (XMMSG_SHM_HELPER_PATH from CMake).
inline pid_t SpawnHelper(const std::vector<std::string>& args) {
  static const std::string kHelper = XMMSG_SHM_HELPER_PATH;
  return SpawnBinary(kHelper, args);
}

// Blocking reap; returns the exit code, or -signal when signalled.
inline int WaitForExit(pid_t pid) {
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return -WTERMSIG(status);
  }
  return -1000;
}

// RAII guard: a helper the test forgets (or an assertion skips past) is
// SIGKILLed and reaped so no child outlives the test binary.
class ChildGuard {
 public:
  explicit ChildGuard(pid_t pid) : pid_(pid) {}
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;
  ~ChildGuard() { Kill(); }

  pid_t pid() const { return pid_; }

  // SIGKILL + reap (idempotent). Returns the wait status of the reap.
  int Kill() {
    if (pid_ <= 0) {
      return 0;
    }
    ::kill(pid_, SIGKILL);
    const int result = WaitForExit(pid_);
    pid_ = -1;
    return result;
  }

  // Graceful reap for helpers expected to exit on their own.
  int Reap() {
    if (pid_ <= 0) {
      return 0;
    }
    const int result = WaitForExit(pid_);
    pid_ = -1;
    return result;
  }

 private:
  pid_t pid_;
};

// Unlinks the named topic segments at scope exit: the library's documented
// policy is never-unlink (shm_segment.hpp), so tests own their cleanup.
class SegmentJanitor {
 public:
  SegmentJanitor(std::string domain_name,
                 std::initializer_list<const char*> topics)
      : key_(xmotion::messaging::detail::DeriveIsolationKey(domain_name)) {
    for (const char* topic : topics) {
      names_.push_back(
          xmotion::messaging::detail::ShmSegmentName(key_, topic));
    }
    // Defensive pre-clean: a crashed previous run with a recycled pid.
    Unlink();
  }
  SegmentJanitor(const SegmentJanitor&) = delete;
  SegmentJanitor& operator=(const SegmentJanitor&) = delete;
  ~SegmentJanitor() { Unlink(); }

 private:
  void Unlink() {
    for (const std::string& name : names_) {
      xmotion::messaging::detail::UnlinkSegment(name);
    }
  }

  std::string key_;
  std::vector<std::string> names_;
};

// Run a shell command, capture its stdout, return the exit code (or -1 on
// popen/abnormal-exit failure). Used to exercise the xmmsg CLI as a real
// subprocess — the CLI binary is the M10 deliverable, so the tests parse
// ITS output, not just the reader library's structs.
inline int RunCommandCaptureStdout(const std::string& command,
                                   std::string* out) {
  out->clear();
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  char buffer[4096];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    out->append(buffer, n);
  }
  const int status = ::pclose(pipe);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

// Minimal JSON well-formedness checker (objects, arrays, strings, numbers,
// true/false/null) — enough to assert the CLI's --json output PARSES, with
// zero dependencies (family rule). Not a validator of anything semantic.
class JsonChecker {
 public:
  static bool Parses(const std::string& text) {
    JsonChecker checker(text);
    checker.SkipWs();
    return checker.Value() && (checker.SkipWs(), checker.at_ == text.size());
  }

 private:
  explicit JsonChecker(const std::string& text) : text_(text) {}

  char Peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
  bool Eat(char c) {
    if (Peek() != c) {
      return false;
    }
    ++at_;
    return true;
  }
  void SkipWs() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' ||
            text_[at_] == '\r')) {
      ++at_;
    }
  }
  bool Literal(const char* word) {
    const std::size_t len = std::char_traits<char>::length(word);
    if (text_.compare(at_, len, word) != 0) {
      return false;
    }
    at_ += len;
    return true;
  }
  bool String() {
    if (!Eat('"')) {
      return false;
    }
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') {
        return true;
      }
      if (c == '\\') {
        if (at_ >= text_.size()) {
          return false;
        }
        ++at_;  // accept any escape; \uXXXX hex digits pass as plain chars
      }
    }
    return false;  // unterminated
  }
  bool Digits() {
    const std::size_t start = at_;
    while (Peek() >= '0' && Peek() <= '9') {
      ++at_;
    }
    return at_ > start;
  }
  bool Number() {
    if (Peek() == '-') {
      ++at_;
    }
    if (!Digits()) {
      return false;
    }
    if (Peek() == '.') {
      ++at_;
      if (!Digits()) {
        return false;
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++at_;
      if (Peek() == '+' || Peek() == '-') {
        ++at_;
      }
      if (!Digits()) {
        return false;
      }
    }
    return true;
  }
  bool Value() {
    SkipWs();
    switch (Peek()) {
      case '{': {
        ++at_;
        SkipWs();
        if (Eat('}')) {
          return true;
        }
        for (;;) {
          SkipWs();
          if (!String()) {
            return false;
          }
          SkipWs();
          if (!Eat(':') || !Value()) {
            return false;
          }
          SkipWs();
          if (Eat('}')) {
            return true;
          }
          if (!Eat(',')) {
            return false;
          }
        }
      }
      case '[': {
        ++at_;
        SkipWs();
        if (Eat(']')) {
          return true;
        }
        for (;;) {
          if (!Value()) {
            return false;
          }
          SkipWs();
          if (Eat(']')) {
            return true;
          }
          if (!Eat(',')) {
            return false;
          }
        }
      }
      case '"':
        return String();
      case 't':
        return Literal("true");
      case 'f':
        return Literal("false");
      case 'n':
        return Literal("null");
      default:
        return Number();
    }
  }

  const std::string& text_;
  std::size_t at_ = 0;
};

}  // namespace shmtest

#endif  // XMMESSAGING_HAS_POSIX_SHM && !XMMSG_SHM_HELPER_BUILD

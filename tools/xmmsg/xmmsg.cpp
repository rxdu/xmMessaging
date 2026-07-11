/*
 * xmmsg — external transport introspection for the POSIX-shm backend (R5,
 * M10): the first thing a user reaches for when "the controller isn't
 * getting plans" happens on a robot. Attaches READ-ONLY to the per-topic
 * segments (detail/introspect_reader.hpp) with zero cooperation from the
 * observed processes; discovery is a /dev/shm scan against the documented
 * name grammar (wire-contract §6.4/§8).
 *
 *   xmmsg list  [--domain KEY] [--json]
 *       enumerate domains + topics: type hash, QoS, payload size,
 *       publisher pid + liveness, subscriber count, last-publish age
 *   xmmsg stat  <topic|segment> [--domain KEY] [--json]
 *       one topic in full: identity, §7 counters per endpoint, accepted
 *       ordinal, last-publish age, deadline misses, refusal record
 *   xmmsg watch <topic|segment> [--domain KEY] [--interval-ms N]
 *       re-print stat every N ms (default 1000) until Ctrl-C — plain
 *       stdout, no curses, pipeable
 *   xmmsg clean [--domain KEY] [--yes] [--json]
 *       reclaim segments whose publisher AND all subscribers are dead
 *       (pid probe) — the never-unlink lifecycle's manual path
 *       (wire-contract §6.4). DRY-RUN by default; --yes unlinks.
 *
 * Scope, per R5: LIVE STATE ONLY. No history, no timelines, no logging to
 * file — anything historical is the telemetry plane's job, offline. This
 * boundary is what keeps the tool from growing into a monitoring GUI.
 *
 * The observer contract: every read path here maps segments PROT_READ and
 * holds no locks (M10-A4/A5). The ONE write this tool can perform is
 * `clean --yes`'s shm_unlink — an explicit operator action on a segment
 * with no live participants, never a write INTO a segment.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdio>

#if !defined(XMMESSAGING_HAS_POSIX_SHM)

int main() {
  std::fprintf(stderr,
               "xmmsg: built without the POSIX shm backend "
               "(XMMESSAGING_WITH_POSIX_SHM=OFF) — nothing to introspect\n");
  return 2;
}

#else

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "xmbase/types/time.hpp"
#include "xmmsg/detail/introspect_reader.hpp"

namespace det = xmotion::messaging::detail;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void OnSigint(int) { g_stop = 1; }

// ---- collected view ---------------------------------------------------------

struct TopicView {
  det::DiscoveredSegment seg;
  det::IntrospectOpenStatus status = det::IntrospectOpenStatus::kUnavailable;
  det::IntrospectSnapshot snap;  // valid only when status == kOk
};

// Discover, validate, snapshot — everything the subcommands render from.
// Foreign files matching the glob are dropped here (validated by magic);
// version-skew and mid-init segments are kept so they can be REPORTED (a
// half-wired or mismatched-build segment is a diagnosis, not noise).
std::vector<TopicView> Collect(const std::string& domain_filter) {
  std::vector<TopicView> views;
  for (det::DiscoveredSegment& seg : det::DiscoverXmmsgSegments()) {
    if (!domain_filter.empty() && seg.isolation_key != domain_filter) {
      continue;
    }
    TopicView view;
    view.seg = std::move(seg);
    det::IntrospectReader reader;
    view.status = det::IntrospectReader::Open(view.seg.name, &reader);
    if (view.status == det::IntrospectOpenStatus::kForeign) {
      continue;  // not ours — skip, never crash on it (M10 discovery rule)
    }
    if (view.status == det::IntrospectOpenStatus::kOk) {
      reader.Snapshot(&view.snap);
    }
    views.push_back(std::move(view));
  }
  return views;
}

// ---- formatting helpers -----------------------------------------------------

std::string HashHex(std::uint64_t hash) {
  char buffer[19];
  std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                static_cast<unsigned long long>(hash));  // §4.1 rendering
  return buffer;
}

std::string QosString(const det::IntrospectSnapshot& snap) {
  if (snap.creator_history_kind == 0) {
    return "latest-only";
  }
  return "queue<" + std::to_string(snap.creator_queue_depth) + ">";
}

std::int64_t NowNs() {
  return ::xmotion::Now().time_since_epoch().count();
}

// Last-publish age (§8: observer age = its own CLOCK_MONOTONIC now minus
// the envelope publish stamp; same host, one clock — R8 measured, never
// advisory). Returns false when the master read gave no stamp.
bool LastPublishAgeUs(const det::IntrospectSnapshot& snap, double* age_us) {
  if (snap.master != det::MasterReadResult::kValue) {
    return false;
  }
  *age_us =
      static_cast<double>(NowNs() - snap.last_publish_stamp_ns) / 1000.0;
  return true;
}

std::string AgeString(const det::IntrospectSnapshot& snap) {
  if (snap.master == det::MasterReadResult::kEmpty) {
    return "never";
  }
  double age_us = 0.0;
  if (!LastPublishAgeUs(snap, &age_us)) {
    return "unknown";  // kStalled: writer died mid-store (M10-A5)
  }
  char buffer[32];
  if (age_us < 10'000.0) {
    std::snprintf(buffer, sizeof(buffer), "%.0f us", age_us);
  } else if (age_us < 10'000'000.0) {
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", age_us / 1000.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f s", age_us / 1'000'000.0);
  }
  return buffer;
}

std::string PubString(const det::IntrospectSnapshot& snap) {
  if (snap.pub_pid == 0) {
    return "none";
  }
  return snap.pub_alive ? "alive" : "DEAD";
}

const char* StatusString(det::IntrospectOpenStatus status) {
  switch (status) {
    case det::IntrospectOpenStatus::kOk:
      return "ok";
    case det::IntrospectOpenStatus::kForeign:
      return "foreign";
    case det::IntrospectOpenStatus::kVersionSkew:
      return "version-skew";
    case det::IntrospectOpenStatus::kNotReady:
      return "not-ready";
    case det::IntrospectOpenStatus::kUnavailable:
      return "unavailable";
  }
  return "?";
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out += buffer;
    } else {
      out += c;
    }
  }
  return out;
}

// ---- JSON rendering (machine consumption; hand-rolled per family rule) ----

void PrintTopicJson(const TopicView& view, const char* indent) {
  const det::IntrospectSnapshot& s = view.snap;
  std::printf("%s{\"topic\":\"%s\",\"segment\":\"%s\",\"status\":\"%s\"",
              indent, JsonEscape(view.seg.topic).c_str(),
              JsonEscape(view.seg.name).c_str(), StatusString(view.status));
  if (view.status != det::IntrospectOpenStatus::kOk) {
    std::printf("}");
    return;
  }
  std::printf(",\"layout_version\":%u,\"envelope_version\":%u",
              s.layout_version, s.envelope_version);
  std::printf(",\"schema_hash\":\"%s\",\"payload_size\":%llu"
              ",\"payload_align\":%llu,\"qos\":\"%s\"",
              HashHex(s.schema_hash).c_str(),
              static_cast<unsigned long long>(s.payload_size),
              static_cast<unsigned long long>(s.payload_align),
              QosString(s).c_str());
  std::printf(",\"publisher\":{\"pid\":%u,\"alive\":%s,\"epoch\":%u,"
              "\"publish_count\":%llu,\"bytes\":%llu}",
              s.pub_pid, s.pub_alive ? "true" : "false", s.pub_epoch,
              static_cast<unsigned long long>(s.pub_publish_count),
              static_cast<unsigned long long>(s.pub_bytes));
  std::printf(",\"accepted_ordinal\":%llu",
              static_cast<unsigned long long>(s.accepted_ordinal));
  double age_us = 0.0;
  const bool have_age = LastPublishAgeUs(s, &age_us);
  std::printf(",\"last_publish\":{\"state\":\"%s\"",
              s.master == det::MasterReadResult::kValue    ? "value"
              : s.master == det::MasterReadResult::kEmpty ? "empty"
                                                          : "stalled");
  if (have_age) {
    std::printf(",\"age_us\":%.1f,\"ordinal\":%llu", age_us,
                static_cast<unsigned long long>(s.last_ordinal));
  }
  std::printf("}");
  std::printf(",\"refusals\":{\"count\":%llu",
              static_cast<unsigned long long>(s.refusal_count));
  if (s.refusal_count > 0) {
    std::printf(",\"last_schema_hash\":\"%s\",\"last_payload_size\":%llu,"
                "\"last_pid\":%u",
                HashHex(s.refused_schema_hash).c_str(),
                static_cast<unsigned long long>(s.refused_payload_size),
                s.refused_pid);
  }
  std::printf("}");
  std::printf(",\"subscribers\":[");
  bool first = true;
  for (const det::IntrospectSubSlot& sub : s.subscribers) {
    std::printf("%s{\"slot\":%u,\"pid\":%u,\"alive\":%s,"
                "\"history\":\"%s\",\"queue_depth\":%u,"
                "\"take_count\":%llu,\"drop_count\":%llu,"
                "\"overwrite_count\":%llu,\"deadline_miss_count\":%llu,"
                "\"last_consumed_ordinal\":%llu}",
                first ? "" : ",", sub.index, sub.pid,
                sub.alive ? "true" : "false",
                sub.history_kind == 0 ? "latest-only" : "queue",
                sub.queue_depth,
                static_cast<unsigned long long>(sub.take_count),
                static_cast<unsigned long long>(sub.drop_count),
                static_cast<unsigned long long>(sub.overwrite_count),
                static_cast<unsigned long long>(sub.deadline_miss_count),
                static_cast<unsigned long long>(sub.last_consumed_ordinal));
    first = false;
  }
  std::printf("]}");
}

// ---- subcommands ------------------------------------------------------------

int CmdList(const std::string& domain_filter, bool json) {
  std::vector<TopicView> views = Collect(domain_filter);
  if (json) {
    std::printf("{\"topics\":[");
    for (std::size_t i = 0; i < views.size(); ++i) {
      if (i > 0) {
        std::printf(",");
      }
      std::printf("\n");
      // Domain key rides on each entry (flat list groups trivially).
      const TopicView& view = views[i];
      std::printf("  {\"domain\":\"%s\",",
                  JsonEscape(view.seg.isolation_key).c_str());
      // Re-open PrintTopicJson's object inline: print without its brace.
      // Simpler: emit the topic object under a key.
      std::printf("\"info\":");
      PrintTopicJson(view, "");
      std::printf("}");
    }
    std::printf("\n]}\n");
    return 0;
  }
  if (views.empty()) {
    std::printf("no xmmsg segments%s in /dev/shm\n",
                domain_filter.empty()
                    ? ""
                    : (" for domain " + domain_filter).c_str());
    return 0;
  }
  // Column widths from content (domains and topics are unbounded names).
  std::size_t w_domain = std::strlen("DOMAIN");
  std::size_t w_topic = std::strlen("TOPIC");
  for (const TopicView& view : views) {
    const std::string& d = view.seg.hashed_name ? view.seg.name
                                                : view.seg.isolation_key;
    const std::string& t =
        view.seg.hashed_name ? std::string("(hashed name)") : view.seg.topic;
    if (d.size() > w_domain) w_domain = d.size();
    if (t.size() > w_topic) w_topic = t.size();
  }
  std::printf("%-*s  %-*s  %-18s  %-12s  %8s  %8s  %-5s  %4s  %s\n",
              static_cast<int>(w_domain), "DOMAIN",
              static_cast<int>(w_topic), "TOPIC", "TYPE-HASH", "QOS",
              "PAYLOAD", "PUB-PID", "PUB", "SUBS", "LAST-PUB");
  for (const TopicView& view : views) {
    const std::string domain = view.seg.hashed_name ? view.seg.name
                                                    : view.seg.isolation_key;
    const std::string topic =
        view.seg.hashed_name ? "(hashed name)" : view.seg.topic;
    if (view.status != det::IntrospectOpenStatus::kOk) {
      std::printf("%-*s  %-*s  [%s]\n", static_cast<int>(w_domain),
                  domain.c_str(), static_cast<int>(w_topic), topic.c_str(),
                  StatusString(view.status));
      continue;
    }
    const det::IntrospectSnapshot& s = view.snap;
    std::printf("%-*s  %-*s  %-18s  %-12s  %8llu  %8u  %-5s  %4u  %s\n",
                static_cast<int>(w_domain), domain.c_str(),
                static_cast<int>(w_topic), topic.c_str(),
                HashHex(s.schema_hash).c_str(), QosString(s).c_str(),
                static_cast<unsigned long long>(s.payload_size), s.pub_pid,
                PubString(s).c_str(), s.active_subscriber_count,
                AgeString(s).c_str());
  }
  return 0;
}

// Resolve one topic argument (a dotted topic name, or a raw segment name
// for hashed-fallback objects) against the discovered set.
bool ResolveTopic(const std::string& arg, const std::string& domain_filter,
                  TopicView* out) {
  std::vector<TopicView> views = Collect(domain_filter);
  std::vector<TopicView> matches;
  const bool raw = !arg.empty() && (arg[0] == '/' || arg.rfind("xmmsg.", 0) == 0);
  const std::string raw_name = arg[0] == '/' ? arg : "/" + arg;
  for (TopicView& view : views) {
    if (raw ? view.seg.name == raw_name : view.seg.topic == arg) {
      matches.push_back(std::move(view));
    }
  }
  if (matches.empty()) {
    std::fprintf(stderr, "xmmsg: no live segment for topic '%s'%s\n",
                 arg.c_str(),
                 domain_filter.empty()
                     ? ""
                     : (" in domain " + domain_filter).c_str());
    return false;
  }
  if (matches.size() > 1) {
    std::fprintf(stderr,
                 "xmmsg: topic '%s' exists in %zu domains — disambiguate "
                 "with --domain:\n",
                 arg.c_str(), matches.size());
    for (const TopicView& view : matches) {
      std::fprintf(stderr, "  --domain %s\n", view.seg.isolation_key.c_str());
    }
    return false;
  }
  *out = std::move(matches.front());
  return true;
}

void PrintStatPlain(const TopicView& view) {
  const det::IntrospectSnapshot& s = view.snap;
  std::printf("topic:            %s\n", view.seg.hashed_name
                                            ? "(hashed name)"
                                            : view.seg.topic.c_str());
  std::printf("domain:           %s\n", view.seg.isolation_key.c_str());
  std::printf("segment:          %s\n", view.seg.name.c_str());
  if (view.status != det::IntrospectOpenStatus::kOk) {
    std::printf("status:           %s\n", StatusString(view.status));
    return;
  }
  std::printf("layout/envelope:  v%u / v%u\n", s.layout_version,
              s.envelope_version);
  std::printf("type hash:        %s\n", HashHex(s.schema_hash).c_str());
  std::printf("payload:          %llu B (align %llu)\n",
              static_cast<unsigned long long>(s.payload_size),
              static_cast<unsigned long long>(s.payload_align));
  std::printf("qos (creator):    %s\n", QosString(s).c_str());
  if (s.pub_pid == 0) {
    std::printf("publisher:        none\n");
  } else {
    std::printf("publisher:        pid %u %s (epoch %u)\n", s.pub_pid,
                s.pub_alive ? "alive" : "DEAD", s.pub_epoch);
  }
  std::printf("published:        %llu msgs, %llu payload bytes\n",
              static_cast<unsigned long long>(s.pub_publish_count),
              static_cast<unsigned long long>(s.pub_bytes));
  std::printf("accepted ordinal: %llu\n",
              static_cast<unsigned long long>(s.accepted_ordinal));
  std::printf("last publish:     %s%s\n", AgeString(s).c_str(),
              s.master == det::MasterReadResult::kValue ? " ago" : "");
  if (s.refusal_count > 0) {
    // M11-A3: both hashes, side by side — the topic's established identity
    // above, the refused endpoint's here.
    std::printf("refusals:         %llu (last: hash %s, payload %llu B, "
                "pid %u) TYPE MISMATCH\n",
                static_cast<unsigned long long>(s.refusal_count),
                HashHex(s.refused_schema_hash).c_str(),
                static_cast<unsigned long long>(s.refused_payload_size),
                s.refused_pid);
  }
  std::printf("subscribers:      %u active / %u slots\n",
              s.active_subscriber_count, s.max_subscribers);
  for (const det::IntrospectSubSlot& sub : s.subscribers) {
    std::printf("  [%2u] pid %-8u %-5s %-11s take=%llu overwrite=%llu "
                "drop=%llu deadline_miss=%llu last_consumed=%llu\n",
                sub.index, sub.pid, sub.alive ? "alive" : "DEAD",
                sub.history_kind == 0
                    ? "latest-only"
                    : ("queue<" + std::to_string(sub.queue_depth) + ">")
                          .c_str(),
                static_cast<unsigned long long>(sub.take_count),
                static_cast<unsigned long long>(sub.overwrite_count),
                static_cast<unsigned long long>(sub.drop_count),
                static_cast<unsigned long long>(sub.deadline_miss_count),
                static_cast<unsigned long long>(sub.last_consumed_ordinal));
  }
}

int CmdStat(const std::string& topic, const std::string& domain_filter,
            bool json) {
  TopicView view;
  if (!ResolveTopic(topic, domain_filter, &view)) {
    return 1;
  }
  if (json) {
    PrintTopicJson(view, "");
    std::printf("\n");
  } else {
    PrintStatPlain(view);
  }
  return 0;
}

int CmdWatch(const std::string& topic, const std::string& domain_filter,
             long interval_ms) {
  std::signal(SIGINT, OnSigint);
  while (g_stop == 0) {
    TopicView view;
    // Re-resolve each tick: segments can appear/disappear under a watcher
    // (that IS the diagnosis sometimes). A vanished topic ends the watch.
    if (!ResolveTopic(topic, domain_filter, &view)) {
      return 1;
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::printf("---- %s ----------------------------------------\n", stamp);
    PrintStatPlain(view);
    std::fflush(stdout);
    struct timespec ts {};
    ts.tv_sec = interval_ms / 1000;
    ts.tv_nsec = (interval_ms % 1000) * 1000000L;
    ::nanosleep(&ts, nullptr);
  }
  return 0;
}

int CmdClean(const std::string& domain_filter, bool yes, bool json) {
  std::vector<TopicView> views = Collect(domain_filter);
  std::vector<const TopicView*> candidates;
  std::size_t skipped = 0;
  for (const TopicView& view : views) {
    if (view.status != det::IntrospectOpenStatus::kOk) {
      // Not validated (mid-init orphan or a build-skew segment): liveness
      // cannot be judged, so it is never auto-reclaimed — reported instead.
      ++skipped;
      continue;
    }
    const det::IntrospectSnapshot& s = view.snap;
    if (s.pub_pid != 0 && s.pub_alive) {
      continue;
    }
    bool any_sub_alive = false;
    for (const det::IntrospectSubSlot& sub : s.subscribers) {
      if (sub.state == det::kShmSubActive && sub.alive) {
        any_sub_alive = true;
        break;
      }
    }
    if (any_sub_alive) {
      continue;
    }
    candidates.push_back(&view);
  }
  if (json) {
    std::printf("{\"candidates\":[");
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      std::printf("%s{\"segment\":\"%s\",\"topic\":\"%s\"}",
                  i > 0 ? "," : "",
                  JsonEscape(candidates[i]->seg.name).c_str(),
                  JsonEscape(candidates[i]->seg.topic).c_str());
    }
    std::printf("],\"skipped_unvalidated\":%zu,\"unlinked\":%s}\n", skipped,
                yes ? "true" : "false");
  }
  int failures = 0;
  for (const TopicView* view : candidates) {
    if (yes) {
      if (::shm_unlink(view->seg.name.c_str()) == 0) {
        if (!json) {
          std::printf("unlinked  %s\n", view->seg.name.c_str());
        }
      } else {
        std::fprintf(stderr, "xmmsg: shm_unlink(%s) failed: %s\n",
                     view->seg.name.c_str(), std::strerror(errno));
        ++failures;
      }
    } else if (!json) {
      std::printf("would unlink  %s  (publisher and all subscribers dead)\n",
                  view->seg.name.c_str());
    }
  }
  if (!json) {
    if (candidates.empty()) {
      std::printf("nothing to clean%s\n",
                  skipped > 0 ? " (unvalidated segments were skipped)" : "");
    } else if (!yes) {
      std::printf("%zu segment(s) reclaimable — dry run; pass --yes to "
                  "unlink\n",
                  candidates.size());
    }
    if (skipped > 0) {
      std::printf("skipped %zu unvalidated segment(s) (not-ready or "
                  "version-skew) — inspect with `xmmsg list`\n",
                  skipped);
    }
  }
  return failures == 0 ? 0 : 1;
}

int Usage() {
  std::fprintf(
      stderr,
      "usage: xmmsg <command> [options]\n"
      "  list  [--domain KEY] [--json]           enumerate live topics\n"
      "  stat  <topic> [--domain KEY] [--json]   one topic's full state\n"
      "  watch <topic> [--domain KEY] [--interval-ms N]\n"
      "                                          re-print stat until Ctrl-C\n"
      "  clean [--domain KEY] [--yes] [--json]   unlink all-dead segments\n"
      "                                          (dry run without --yes)\n"
      "KEY is the domain isolation key (wire-contract §6.2), e.g. "
      "u1000.nav_stack.\nLive state only (R5): history belongs to the "
      "telemetry plane.\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return Usage();
  }
  const std::string command = argv[1];
  std::string domain_filter;
  std::string topic;
  bool json = false;
  bool yes = false;
  long interval_ms = 1000;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0) {
      json = true;
    } else if (std::strcmp(argv[i], "--yes") == 0) {
      yes = true;
    } else if (std::strcmp(argv[i], "--domain") == 0 && i + 1 < argc) {
      domain_filter = argv[++i];
    } else if (std::strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
      interval_ms = std::strtol(argv[++i], nullptr, 10);
      if (interval_ms <= 0) {
        return Usage();
      }
    } else if (argv[i][0] != '-' && topic.empty()) {
      topic = argv[i];
    } else {
      return Usage();
    }
  }
  if (command == "list") {
    return topic.empty() && !yes ? CmdList(domain_filter, json) : Usage();
  }
  if (command == "stat") {
    return !topic.empty() && !yes ? CmdStat(topic, domain_filter, json)
                                  : Usage();
  }
  if (command == "watch") {
    return !topic.empty() && !yes && !json
               ? CmdWatch(topic, domain_filter, interval_ms)
               : Usage();
  }
  if (command == "clean") {
    return topic.empty() ? CmdClean(domain_filter, yes, json) : Usage();
  }
  return Usage();
}

#endif  // XMMESSAGING_HAS_POSIX_SHM

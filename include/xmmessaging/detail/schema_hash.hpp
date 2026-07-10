/*
 * detail/schema_hash.hpp
 *
 * R6 type-identity seam: every endpoint carries a 64-bit schema hash of its
 * payload; endpoint matching compares hashes and refuses a mismatch with
 * AdvertiseStatus/SubscribeStatus::kTypeMismatch.
 *
 * The hash FUNCTION is final: FNV-1a 64 over an ASCII description, exactly
 * as specified in docs/wire-contract.md §4.1 (conformance cross-checks:
 * fnv1a64("") == 0xCBF29CE484222325).
 *
 * The hash INPUT is interim at P0b: C++17 has no portable field reflection,
 * so SchemaHashOf<T>() hashes typeid(T).name() + sizeof + alignof instead of
 * the wire-contract §4.2 canonical type description. This is sufficient for
 * the in-process reach (both endpoints live in one binary, so type identity
 * is real identity), but it is NOT the published wire contract:
 *
 *   TODO(P1): replace the interim description with the §4.2 canonical
 *   field-description string (names, types, absolute offsets, sizes) and
 *   validate against conformance vectors V1-V6 (§5) before any endpoint
 *   hash ever crosses a process boundary. The seam is this one function.
 *
 * detail/: not part of the portable API surface.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <typeinfo>

namespace xmotion {
namespace messaging {
namespace detail {

// FNV-1a 64-bit parameters (wire-contract §4.1 — normative).
inline constexpr std::uint64_t kFnv1aOffsetBasis = 0xCBF29CE484222325ULL;
inline constexpr std::uint64_t kFnv1aPrime = 0x100000001B3ULL;

// FNV-1a 64 over a byte string. constexpr-friendly so the P1 canonical
// description (a compile-time string) can hash at compile time.
constexpr std::uint64_t Fnv1a64(std::string_view bytes,
                                std::uint64_t hash = kFnv1aOffsetBasis) noexcept {
  for (char c : bytes) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnv1aPrime;
  }
  return hash;
}

// The R6 schema hash of a payload type, computed once per type per process.
//
// INTERIM (P0b): hashes the compiler's type name + size + alignment, not the
// wire-contract §4.2 canonical field description — see the header comment.
// The "xmmsg-interim-schema:" prefix guarantees no interim hash can ever
// collide with a conforming §4 hash by accident of format.
template <typename T>
std::uint64_t SchemaHashOf() {
  static const std::uint64_t kHash = [] {
    std::string description("xmmsg-interim-schema:");
    description += typeid(T).name();
    description += ':';
    description += std::to_string(sizeof(T));
    description += ':';
    description += std::to_string(alignof(T));
    return Fnv1a64(description);
  }();
  return kHash;
}

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

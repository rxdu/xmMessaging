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
 * The hash INPUT has two forms (P1b decision, recorded):
 *
 *   1. CANONICAL (wire-contract §4.2) — for payload types that opt in via
 *      XMMSG_DESCRIBE below. C++17 has no field reflection, so the field
 *      list is declared once, manually, next to the type; the machinery
 *      derives names, spec type tokens, absolute offsets, and sizes, builds
 *      the §4.2 canonical description string, verifies the §4.2 rule-10
 *      tiling (explicit padding, M12-A5), and hashes it. Validated against
 *      the §5 conformance vectors V1–V6 (test/behavioral/
 *      wire_schema_test.cpp). This is the form that is build-stable AND
 *      language-neutral; every payload that crosses a process boundary
 *      SHOULD use it.
 *
 *   2. INTERIM (typeid name + sizeof + alignof) — for types that do not
 *      opt in. Build-stable across separate builds of the SAME toolchain
 *      family (the Itanium ABI mangled name is a function of the type, not
 *      the build), so it is usable across processes on the tested
 *      baselines. STATED DIVERGENCE from R6/§4: it is not computable by a
 *      foreign language, and it does NOT catch a same-name/same-size field
 *      reorder (the M11-A2 case) — the canonical form does. The
 *      "xmmsg-interim-schema:" prefix guarantees the two forms can never
 *      collide by accident of format.
 *
 * detail/: not part of the portable API surface — except XMMSG_DESCRIBE /
 * XMMSG_FIELD, which payload-owning code invokes (at GLOBAL namespace
 * scope) to opt a type into the canonical form.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace xmotion {
namespace messaging {
namespace detail {

// FNV-1a 64-bit parameters (wire-contract §4.1 — normative).
inline constexpr std::uint64_t kFnv1aOffsetBasis = 0xCBF29CE484222325ULL;
inline constexpr std::uint64_t kFnv1aPrime = 0x100000001B3ULL;

// FNV-1a 64 over a byte string. constexpr-friendly so a compile-time
// description could hash at compile time.
constexpr std::uint64_t Fnv1a64(std::string_view bytes,
                                std::uint64_t hash = kFnv1aOffsetBasis) noexcept {
  for (char c : bytes) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnv1aPrime;
  }
  return hash;
}

// ---------------------------------------------------------------------------
// Canonical type description (wire-contract §4.2) for XMMSG_DESCRIBE types.
// ---------------------------------------------------------------------------

// Build context: the description text plus the leaf extents in declaration
// order, for the §4.2 rule-10 completeness check.
struct SchemaContext {
  std::string text;
  std::vector<std::pair<std::size_t, std::size_t>> extents;  // offset, size
};

// Primary template: specialized by XMMSG_DESCRIBE. A specialization
// provides `static void AppendFields(SchemaContext&, const std::string&
// prefix, std::size_t base_offset)`.
template <typename T>
struct WireSchema;

template <typename T, typename = void>
struct IsWireDescribed : std::false_type {};
template <typename T>
struct IsWireDescribed<
    T, std::void_t<decltype(WireSchema<T>::AppendFields(
           std::declval<SchemaContext&>(), std::declval<const std::string&>(),
           std::size_t{0}))>> : std::true_type {};

// §4.2 rule-5 type vocabulary: spec token per permitted scalar type.
// bool canonicalizes as u8, enums as their fixed underlying type (§3).
template <typename T, typename = void>
struct ScalarToken;
template <>
struct ScalarToken<std::uint8_t> { static constexpr const char* value = "u8"; };
template <>
struct ScalarToken<std::uint16_t> { static constexpr const char* value = "u16"; };
template <>
struct ScalarToken<std::uint32_t> { static constexpr const char* value = "u32"; };
template <>
struct ScalarToken<std::uint64_t> { static constexpr const char* value = "u64"; };
template <>
struct ScalarToken<std::int8_t> { static constexpr const char* value = "i8"; };
template <>
struct ScalarToken<std::int16_t> { static constexpr const char* value = "i16"; };
template <>
struct ScalarToken<std::int32_t> { static constexpr const char* value = "i32"; };
template <>
struct ScalarToken<std::int64_t> { static constexpr const char* value = "i64"; };
template <>
struct ScalarToken<float> { static constexpr const char* value = "f32"; };
template <>
struct ScalarToken<double> { static constexpr const char* value = "f64"; };
template <>
struct ScalarToken<bool> { static constexpr const char* value = "u8"; };
template <typename T>
struct ScalarToken<T, std::enable_if_t<std::is_enum_v<T>>>
    : ScalarToken<std::underlying_type_t<T>> {};

template <typename T, typename = void>
struct HasScalarToken : std::false_type {};
template <typename T>
struct HasScalarToken<T, std::void_t<decltype(ScalarToken<T>::value)>>
    : std::true_type {};

// Array unwrapping: base scalar/struct type + row-major dimension suffixes
// in declaration order (§4.2 rule 6).
template <typename T>
struct ArrayShape {
  using Base = T;
  static void AppendDims(std::string&) {}
};
template <typename E, std::size_t N>
struct ArrayShape<E[N]> {
  using Base = typename ArrayShape<E>::Base;
  static void AppendDims(std::string& out) {
    out += '[';
    out += std::to_string(N);
    out += ']';
    ArrayShape<E>::AppendDims(out);
  }
};

// One leaf field line (§4.2 rule 3): `name:type:offset:size`, absolute
// offsets, decimal, LF-terminated.
template <typename T>
void AppendLeafLine(SchemaContext& ctx, const std::string& name,
                    std::size_t offset) {
  using Base = typename ArrayShape<T>::Base;
  ctx.text += name;
  ctx.text += ':';
  ctx.text += ScalarToken<Base>::value;
  ArrayShape<T>::AppendDims(ctx.text);
  ctx.text += ':';
  ctx.text += std::to_string(offset);
  ctx.text += ':';
  ctx.text += std::to_string(sizeof(T));
  ctx.text += '\n';
  ctx.extents.emplace_back(offset, sizeof(T));
}

// Field dispatch: scalar / array-of-scalar leaves become one line; a
// described nested struct expands recursively with dotted absolute-offset
// paths (§4.2 rule 7); an array of described structs expands per element
// (§4.2 rule 8).
template <typename T>
void DescribeField(SchemaContext& ctx, const std::string& prefix,
                   const std::string& name, std::size_t offset) {
  using Base = typename ArrayShape<T>::Base;
  if constexpr (IsWireDescribed<Base>::value) {
    if constexpr (std::is_array_v<T>) {
      using Elem = std::remove_extent_t<T>;
      constexpr std::size_t kN = std::extent_v<T>;
      static_assert(kN > 0, "zero-length arrays are forbidden (§4.2 rule 6)");
      for (std::size_t i = 0; i < kN; ++i) {
        DescribeField<Elem>(ctx, prefix,
                            name + "[" + std::to_string(i) + "]",
                            offset + i * sizeof(Elem));
      }
    } else {
      WireSchema<T>::AppendFields(ctx, prefix + name + ".", offset);
    }
  } else {
    static_assert(HasScalarToken<Base>::value,
                  "xmMessaging: field type is not in the wire-contract §4.2 "
                  "vocabulary (fixed-width integers, f32/f64, fixed arrays, "
                  "or XMMSG_DESCRIBE'd nested structs)");
    AppendLeafLine<T>(ctx, prefix + name, offset);
  }
}

// §4.2 rule 10: leaf extents, in declaration order, must be strictly
// increasing, non-overlapping, and exactly tile [0, size) — the explicit-
// padding rule (§3) as a checkable fact.
inline bool ExtentsTile(const SchemaContext& ctx, std::size_t size) {
  std::size_t at = 0;
  for (const auto& [offset, extent] : ctx.extents) {
    if (offset != at) {
      return false;
    }
    at = offset + extent;
  }
  return at == size;
}

// The §4.2 canonical description of a described payload type.
template <typename T>
std::string CanonicalDescription() {
  static_assert(IsWireDescribed<T>::value,
                "CanonicalDescription requires an XMMSG_DESCRIBE'd type");
  static_assert(std::is_standard_layout_v<T>,
                "wire payloads must be standard-layout (§3)");
  SchemaContext ctx;
  ctx.text = "size:" + std::to_string(sizeof(T)) + "\n";
  WireSchema<T>::AppendFields(ctx, std::string(), 0);
  // M12-A5: implicit padding poisons both the hash's meaning and zero-copy
  // reads — asserted at the wiring site that computes the hash.
  assert(ExtentsTile(ctx, sizeof(T)) &&
         "xmMessaging: XMMSG_DESCRIBE field list does not tile the payload "
         "— implicit padding or a missing field (wire-contract §3/§4.2 "
         "rule 10); declare explicit _padN fields");
  return ctx.text;
}

// The R6 schema hash of a payload type, computed once per type per process.
// Canonical §4 form for XMMSG_DESCRIBE'd types; the documented interim form
// otherwise (see the header comment for the divergence).
template <typename T>
std::uint64_t SchemaHashOf() {
  if constexpr (IsWireDescribed<T>::value) {
    static const std::uint64_t kHash = Fnv1a64(CanonicalDescription<T>());
    return kHash;
  } else {
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
}

}  // namespace detail
}  // namespace messaging
}  // namespace xmotion

// ---------------------------------------------------------------------------
// XMMSG_DESCRIBE(Type, XMMSG_FIELD(a), XMMSG_FIELD(b), ...)
//
// Opts `Type` into the wire-contract §4.2 canonical schema description.
// Invoke at GLOBAL namespace scope (the macro opens the library namespace),
// with Type fully qualified and every leaf-or-nested field listed IN
// DECLARATION ORDER, explicit padding fields included (§3). The rule-10
// tiling check catches omissions and implicit padding at hash time.
// ---------------------------------------------------------------------------
#define XMMSG_DESCRIBE(Type, ...)                                            \
  namespace xmotion {                                                        \
  namespace messaging {                                                      \
  namespace detail {                                                         \
  template <>                                                                \
  struct WireSchema<Type> {                                                  \
    using XmmsgSelf = Type;                                                  \
    static void AppendFields(SchemaContext& ctx,                             \
                             const std::string& xmmsg_prefix,                \
                             std::size_t xmmsg_base) {                       \
      __VA_ARGS__;                                                           \
    }                                                                        \
  };                                                                         \
  }                                                                          \
  }                                                                          \
  }                                                                          \
  static_assert(true, "XMMSG_DESCRIBE requires a trailing semicolon")

#define XMMSG_FIELD(field)                                                   \
  ::xmotion::messaging::detail::DescribeField<decltype(XmmsgSelf::field)>(   \
      ctx, xmmsg_prefix, #field, xmmsg_base + offsetof(XmmsgSelf, field))

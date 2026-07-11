/*
 * payload_traits.hpp
 *
 * Payload requirements by reach as compile-time facts (design.md "The
 * message vocabulary", M6-A4, M12-A5): in-process accepts any movable C++
 * type; zero-copy requires trivially copyable + fixed size; cross-language
 * topics additionally require standard layout with explicit padding.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <type_traits>

namespace xmotion {
namespace messaging {

// The floor every reach shares (design.md payload requirements): a plain
// movable object type. Asserted at Advertise<T>/Subscribe<T> instantiation.
template <typename T>
inline constexpr bool is_payload_v =
    std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> &&
    std::is_move_constructible_v<T>;

// Zero-copy path (QoS knob "Loan", M6-A4): the payload is constructed in
// transport memory and read in place, so it must be trivially copyable.
// Asserted at Publisher<T>::Loan() use (the minting verb — deliberately not
// at Loan<T> class scope, so Publish overload resolution can complete the
// type for non-trivially-copyable payloads).
template <typename T>
inline constexpr bool is_zero_copy_payload_v = std::is_trivially_copyable_v<T>;

// Cross-language topics (R10, M12-A5): standard layout, trivially copyable.
// NOTE (P0a open item): the "no implicit padding" half of the rule is not
// expressible as a portable C++17 trait for float-bearing payloads
// (has_unique_object_representations is false for IEEE floats); it is
// enforced by the R6 schema-hash machinery at P1, which knows field
// offsets and sizes.
template <typename T>
inline constexpr bool is_cross_language_payload_v =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

}  // namespace messaging
}  // namespace xmotion

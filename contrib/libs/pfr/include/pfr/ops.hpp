// Copyright (c) 2016-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef PFR_OPS_HPP
#define PFR_OPS_HPP
#pragma once

#include <pfr/detail/config.hpp>

#if !defined(PFR_USE_MODULES) || defined(PFR_INTERFACE_UNIT)

#include <pfr/detail/detectors.hpp>
#include <pfr/ops_fields.hpp>

/// \file pfr/ops.hpp
/// Contains comparison and hashing functions.
/// If type is comparable using its own operator or its conversion operator, then the types operator is used. Otherwise
/// the operation is done via corresponding function from pfr/ops.hpp header.
///
/// \b Example:
/// \code
///     #include <pfr/ops.hpp>
///     struct comparable_struct {      // No operators defined for that structure
///         int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///     };
///     // ...
///
///     comparable_struct s1 {0, 1, "Hello", false, 6,7,8,9,10,11};
///     comparable_struct s2 {0, 1, "Hello", false, 6,7,8,9,10,11111};
///     assert(pfr::lt(s1, s2));
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:
namespace pfr {

namespace detail {

///////////////////// Helper typedefs that are used by all the ops
    template <template <class, class> class Detector, class T, class U>
    using enable_not_comp_base_t = std::enable_if_t<
        not_applicable<Detector, T const&, U const&>::value,
        bool
    >;

    template <template <class, class> class Detector, class T, class U>
    using enable_comp_base_t = std::enable_if_t<
        !not_applicable<Detector, T const&, U const&>::value,
        bool
    >;
///////////////////// std::enable_if_t like functions that enable only if types do not support operation

    template <class T, class U> using enable_not_eq_comp_t = enable_not_comp_base_t<comp_eq_detector, T, U>;
    template <class T, class U> using enable_not_ne_comp_t = enable_not_comp_base_t<comp_ne_detector, T, U>;
    template <class T, class U> using enable_not_lt_comp_t = enable_not_comp_base_t<comp_lt_detector, T, U>;
    template <class T, class U> using enable_not_le_comp_t = enable_not_comp_base_t<comp_le_detector, T, U>;
    template <class T, class U> using enable_not_gt_comp_t = enable_not_comp_base_t<comp_gt_detector, T, U>;
    template <class T, class U> using enable_not_ge_comp_t = enable_not_comp_base_t<comp_ge_detector, T, U>;

    template <class T> using enable_not_hashable_t = std::enable_if_t<
        not_applicable<hash_detector, const T&, const T&>::value,
        std::size_t
    >;
///////////////////// std::enable_if_t like functions that enable only if types do support operation

    template <class T, class U> using enable_eq_comp_t = enable_comp_base_t<comp_eq_detector, T, U>;
    template <class T, class U> using enable_ne_comp_t = enable_comp_base_t<comp_ne_detector, T, U>;
    template <class T, class U> using enable_lt_comp_t = enable_comp_base_t<comp_lt_detector, T, U>;
    template <class T, class U> using enable_le_comp_t = enable_comp_base_t<comp_le_detector, T, U>;
    template <class T, class U> using enable_gt_comp_t = enable_comp_base_t<comp_gt_detector, T, U>;
    template <class T, class U> using enable_ge_comp_t = enable_comp_base_t<comp_ge_detector, T, U>;

    template <class T> using enable_hashable_t = std::enable_if_t<
        !not_applicable<hash_detector, const T&, const T&>::value,
        std::size_t
    >;
} // namespace detail

PFR_BEGIN_MODULE_EXPORT

/// \brief Compares lhs and rhs for equality using their own comparison and conversion operators; if no operators available returns \forcedlink{eq_fields}(lhs, rhs).
///
/// \returns true if lhs is equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_eq_comp_t<T, U> eq(const T& lhs, const U& rhs) noexcept {
    return pfr::eq_fields(lhs, rhs);
}

/// \overload eq
template <class T, class U>
constexpr detail::enable_eq_comp_t<T, U> eq(const T& lhs, const U& rhs) {
    return lhs == rhs;
}


/// \brief Compares lhs and rhs for inequality using their own comparison and conversion operators; if no operators available returns \forcedlink{ne_fields}(lhs, rhs).
///
/// \returns true if lhs is not equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_ne_comp_t<T, U> ne(const T& lhs, const U& rhs) noexcept {
    return pfr::ne_fields(lhs, rhs);
}

/// \overload ne
template <class T, class U>
constexpr detail::enable_ne_comp_t<T, U> ne(const T& lhs, const U& rhs) {
    return lhs != rhs;
}


/// \brief Compares lhs and rhs for less-than using their own comparison and conversion operators; if no operators available returns \forcedlink{lt_fields}(lhs, rhs).
///
/// \returns true if lhs is less than rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_lt_comp_t<T, U> lt(const T& lhs, const U& rhs) noexcept {
    return pfr::lt_fields(lhs, rhs);
}

/// \overload lt
template <class T, class U>
constexpr detail::enable_lt_comp_t<T, U> lt(const T& lhs, const U& rhs) {
    return lhs < rhs;
}


/// \brief Compares lhs and rhs for greater-than using their own comparison and conversion operators; if no operators available returns \forcedlink{lt_fields}(lhs, rhs).
///
/// \returns true if lhs is greater than rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_gt_comp_t<T, U> gt(const T& lhs, const U& rhs) noexcept {
    return pfr::gt_fields(lhs, rhs);
}

/// \overload gt
template <class T, class U>
constexpr detail::enable_gt_comp_t<T, U> gt(const T& lhs, const U& rhs) {
    return lhs > rhs;
}


/// \brief Compares lhs and rhs for less-equal using their own comparison and conversion operators; if no operators available returns \forcedlink{le_fields}(lhs, rhs).
///
/// \returns true if lhs is less or equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_le_comp_t<T, U> le(const T& lhs, const U& rhs) noexcept {
    return pfr::le_fields(lhs, rhs);
}

/// \overload le
template <class T, class U>
constexpr detail::enable_le_comp_t<T, U> le(const T& lhs, const U& rhs) {
    return lhs <= rhs;
}


/// \brief Compares lhs and rhs for greater-equal using their own comparison and conversion operators; if no operators available returns \forcedlink{ge_fields}(lhs, rhs).
///
/// \returns true if lhs is greater or equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_ge_comp_t<T, U> ge(const T& lhs, const U& rhs) noexcept {
    return pfr::ge_fields(lhs, rhs);
}

/// \overload ge
template <class T, class U>
constexpr detail::enable_ge_comp_t<T, U> ge(const T& lhs, const U& rhs) {
    return lhs >= rhs;
}


/// \brief Hashes value using its own std::hash specialization; if no std::hash specialization available returns \forcedlink{hash_fields}(value).
///
/// \returns std::size_t with hash of the value
template <class T>
constexpr detail::enable_not_hashable_t<T> hash_value(const T& value) noexcept {
    return pfr::hash_fields(value);
}

/// \overload hash_value
template <class T>
constexpr detail::enable_hashable_t<T> hash_value(const T& value) {
    return std::hash<T>{}(value);
}

PFR_END_MODULE_EXPORT

} // namespace pfr

#endif  // #if !defined(PFR_USE_MODULES) || defined(PFR_INTERFACE_UNIT)

#endif // PFR_OPS_HPP

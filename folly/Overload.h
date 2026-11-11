/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/functional/Invoke.h>

/**
 * folly implementation of `std::overload` like functionality
 *
 * Example:
 *  struct One {};
 *  struct Two {};
 *  std::variant<One, Two> value;
 *
 *  variant_match(value,
 *    [] (const One& one) { ... },
 *    [] (const Two& two) { ... });
 */

namespace folly {

namespace detail {

// MSVC does not implement noexcept deduction https://godbolt.org/z/Mxdjao1q6
#if !defined(_MSC_VER)
#define FOLLY_DETAIL_NOEXCEPT_SPECIFICATION noexcept(Noexcept)
#define FOLLY_DETAIL_NOEXCEPT_DECLARATION bool Noexcept,
#else
#define FOLLY_DETAIL_NOEXCEPT_SPECIFICATION
#define FOLLY_DETAIL_NOEXCEPT_DECLARATION
#endif

template <typename T>
struct FunctionClassType {
  using type = T;
};

// You cannot derive from a pointer to function, so wrap it in a class

template <FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return, typename... Args>
struct FunctionClassType<Return (*)(
    Args...) FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr = Return (*)(Args...) FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr function) noexcept
        : function_(function) {}
    constexpr auto operator()(Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return function_(std::forward<Args>(args)...);
    }

   private:
    Ptr function_;
  };
};

// You cannot derive from a pointer to member function, so wrap it in a class.
// This cannot be implemented with
// `std::enable_if_t<std::is_member_pointer_v<T>>` because you don't get
// preferred overload resolution on the object type to match const / ref
// qualifiers.

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<Return (Self::*)(
    Args...) FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr = Return (Self::*)(Args...) FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(Self& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (self.*memberPointer_)(std::forward<Args>(args)...);
    }
    constexpr auto operator()(Self&& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (self.*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<Return (Self::*)(
    Args...) const FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr =
      Return (Self::*)(Args...) const FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(const Self& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (self.*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<
    Return (Self::*)(Args...) & FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr = Return (Self::*)(Args...) & FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(Self& self, Args&&... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (self.*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<
    Return (Self::*)(Args...) const & FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr =
      Return (Self::*)(Args...) const& FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(const Self& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (self.*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<
    Return (Self::*)(Args...) && FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr = Return (Self::*)(Args...) && FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(Self&& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (std::move(self).*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <
    FOLLY_DETAIL_NOEXCEPT_DECLARATION typename Return,
    typename Self,
    typename... Args>
struct FunctionClassType<
    Return (Self::*)(Args...) const && FOLLY_DETAIL_NOEXCEPT_SPECIFICATION> {
  using Ptr =
      Return (Self::*)(Args...) const&& FOLLY_DETAIL_NOEXCEPT_SPECIFICATION;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(const Self&& self, Args... args) const
        FOLLY_DETAIL_NOEXCEPT_SPECIFICATION->Return {
      return (std::move(self).*memberPointer_)(std::forward<Args>(args)...);
    }

   private:
    Ptr memberPointer_;
  };
};

template <typename T, typename Self>
struct FunctionClassType<T Self::*> {
  using Ptr = T Self::*;
  struct type {
    /* implicit */ constexpr type(Ptr memberPointer) noexcept
        : memberPointer_(memberPointer) {}
    constexpr auto operator()(Self& self) const noexcept -> T& {
      return self.*memberPointer_;
    }
    constexpr auto operator()(const Self& self) const noexcept -> const T& {
      return self.*memberPointer_;
    }
    constexpr auto operator()(Self&& self) const noexcept -> T&& {
      return std::move(self).*memberPointer_;
    }
    constexpr auto operator()(const Self&& self) const noexcept -> const T&& {
      return std::move(self).*memberPointer_;
    }

   private:
    Ptr memberPointer_;
  };
};

#undef FOLLY_DETAIL_NOEXCEPT_DECLARATION
#undef FOLLY_DETAIL_NOEXCEPT_SPECIFICATION

template <typename...>
struct Overload {};

template <typename Case, typename... Cases>
struct Overload<Case, Cases...> : Overload<Cases...>, Case {
  explicit constexpr Overload(Case c, Cases... cs)
      : Overload<Cases...>(std::move(cs)...), Case(std::move(c)) {}

  using Case::operator();
  using Overload<Cases...>::operator();
};

template <typename Case>
struct Overload<Case> : Case {
  explicit constexpr Overload(Case c) : Case(std::move(c)) {}

  using Case::operator();
};
} // namespace detail

/*
 * Combine multiple `Cases` in one function object
 *
 * Each element of `Cases` must be a class type with `operator()`, a pointer to
 * a function, a pointer to a member function, or a pointer to member data.
 * `final` types and pointers to `volatile`-qualified member functions are not
 * supported. If the `Case` type is a pointer to member, the first argument must
 * be a class type or reference to class type (pointer to class type is not
 * supported).
 */
template <typename... Cases>
constexpr decltype(auto) overload(Cases&&... cases) {
  return detail::Overload<typename detail::FunctionClassType<
      typename std::decay<Cases>::type>::type...>{
      std::forward<Cases>(cases)...};
}

namespace detail {
// Converts a type derived from `std::variant` to its `std::variant` base, as
// P2162 specifies for `std::visit`, for standard libraries that lack P2162.
template <typename... T>
constexpr std::variant<T...>& as_std_variant(std::variant<T...>& v) noexcept {
  return v;
}
template <typename... T>
constexpr const std::variant<T...>& as_std_variant(
    const std::variant<T...>& v) noexcept {
  return v;
}
template <typename... T>
constexpr std::variant<T...>&& as_std_variant(std::variant<T...>&& v) noexcept {
  return std::move(v);
}
template <typename... T>
constexpr const std::variant<T...>&& as_std_variant(
    const std::variant<T...>&& v) noexcept {
  return std::move(v);
}
} // namespace detail

/*
 * Match `Variant` with one of the `Cases`
 *
 * Note: you can also use `[] (const auto&) {...}` as default case
 *
 * `Variant` must be a `std::variant` or a type derived from one, which is
 * matched as its `std::variant` base. Dispatches through `std::visit`.
 */
template <typename Variant, typename... Cases>
decltype(auto) variant_match(Variant&& variant, Cases&&... cases) {
  return std::visit(
      overload(std::forward<Cases>(cases)...),
      detail::as_std_variant(std::forward<Variant>(variant)));
}

template <typename R, typename Variant, typename... Cases>
R variant_match(Variant&& variant, Cases&&... cases) {
  auto f = [&](auto&& v) -> R {
    if constexpr (std::is_void<R>::value) {
      overload(std::forward<Cases>(cases)...)(std::forward<decltype(v)>(v));
    } else {
      return overload(std::forward<Cases>(cases)...)(
          std::forward<decltype(v)>(v));
    }
  };
  return std::visit(f, detail::as_std_variant(std::forward<Variant>(variant)));
}

} // namespace folly

//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>

#if defined(_MSC_VER) && defined(_M_IX86)
#define FOLLY_FCONTEXT_CALLDECL __cdecl
#else
#define FOLLY_FCONTEXT_CALLDECL
#endif

namespace folly::detail::fcontext {

using fcontext_t = void*;

struct transfer_t {
  fcontext_t fctx;
  void* data;
};

extern "C" transfer_t FOLLY_FCONTEXT_CALLDECL
folly_jump_fcontext(fcontext_t to, void* vp);
extern "C" fcontext_t FOLLY_FCONTEXT_CALLDECL
folly_make_fcontext(void* sp, std::size_t size, void (*fn)(transfer_t));

} // namespace folly::detail::fcontext

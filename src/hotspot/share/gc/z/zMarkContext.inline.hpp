/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZMARKCONTEXT_INLINE_HPP
#define SHARE_GC_Z_ZMARKCONTEXT_INLINE_HPP

#include "gc/z/zGlobals.hpp"
#include "gc/z/zMarkContext.hpp"
#include "runtime/os.hpp"

constexpr size_t ZMarkContext::nvictim_stripes() const {
  // Steal work from at most three other stripes
  return 3;
}

constexpr bool ZMarkContext::should_timeout() const {
  // Never times out
  return false;
}

constexpr size_t ZMarkEndContext::nvictim_stripes() const {
  // Steal work from all other stripes
  return ZMarkStripesMax;
}

inline bool ZMarkEndContext::should_timeout() {
  if (++_timeout_check_count == _timeout_check_at) {
    const uint64_t now = os::elapsed_counter();
    if (now >= _timeout_end) {
      _timeout_expired = true;
    } else {
      _timeout_check_at += _timeout_check_interval;
    }
  }

  return _timeout_expired;
}

#endif // SHARE_GC_Z_ZMARKCONTEXT_INLINE_HPP

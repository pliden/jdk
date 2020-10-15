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

#include "precompiled.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zMarkContext.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/timer.hpp"

ZMarkEndContext::ZMarkEndContext() :
    _timeout_start(os::elapsed_counter()),
    _timeout_end(_timeout_start + TimeHelper::micros_to_counter(ZMarkEndTimeout)),
    _timeout_check_interval(100),
    _timeout_check_count(0),
    _timeout_check_at(_timeout_check_interval),
    _timeout_expired(false) {}

ZMarkEndContext::~ZMarkEndContext() {
  const uint64_t duration = os::elapsed_counter() - _timeout_start;
  log_debug(gc, marking)("Mark End: %s, " UINT64_FORMAT " oops, %.3fms",
                         _timeout_expired ? "Timed out" : "Completed",
                         _timeout_check_count,
                         TimeHelper::counter_to_millis(duration));
}

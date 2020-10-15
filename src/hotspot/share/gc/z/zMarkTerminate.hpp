/*
 * Copyright (c) 2017, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMARKTERMINATE_HPP
#define SHARE_GC_Z_ZMARKTERMINATE_HPP

#include "gc/z/zMarkStack.hpp"
#include "gc/z/zMarkTerminateState.hpp"

class ZMarkTerminate {
private:
  static const uint _terminate = (uint)-1;

  ZMarkTerminateState _state;

  bool enter_idle_mode(ZMarkStripeMap stripe_map);
  bool exit_idle_mode(ZMarkStripeMap stripe_map);
  bool enter_terminate_mode();

public:
  void reset(uint nworkers);
  void set_active_stripes(ZMarkStripeMap stripe_map);
  bool has_active_stripes() const;
  bool idle(ZMarkStripeMap stripe_map);
};

#endif // SHARE_GC_Z_ZMARKTERMINATE_HPP

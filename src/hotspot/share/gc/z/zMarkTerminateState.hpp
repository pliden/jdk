/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMARKTERMINATESTATE_HPP
#define SHARE_GC_Z_ZMARKTERMINATESTATE_HPP

#include "gc/z/zBitField.hpp"
#include "metaprogramming/primitiveConversions.hpp"
#include <type_traits>

//
// Mark terminate state layout
// ---------------------------
//
//   6                                 3 3
//   3                                 2 1                                 0
//  +-----------------------------------+-----------------------------------+
//  |11111111 11111111 11111111 11111111|11111111 11111111 11111111 11111111|
//  +-----------------------------------+-----------------------------------+
//  |                                   |
//  |                                   * 31-0 Active stripe flags (32-bits)
//  |
//  * 63-32 Number of active workers (32-bits)
//

class ZMarkTerminateState {
  friend struct PrimitiveConversions;

private:
  typedef ZBitField<uint64_t, uint32_t, 0,  32> field_active_stripes;
  typedef ZBitField<uint64_t, uint,     32, 32> field_nactive_workers;

  uint64_t _state;

public:
  ZMarkTerminateState() :
      _state(0) {}

  ZMarkTerminateState(uint nactive_workers, uint32_t active_stripes) :
      _state(field_nactive_workers::encode(nactive_workers) |
             field_active_stripes::encode(active_stripes)) {}

  uint32_t active_stripes() const {
    return field_active_stripes::decode(_state);
  }

  uint nactive_workers() const {
    return field_nactive_workers::decode(_state);
  }

  bool is_cleared() const {
    return _state == 0;
  }

  bool operator==(const ZMarkTerminateState& other) const {
    return _state == other._state;
  }
};

// Needed to allow atomic operations on ZMarkTerminateState
template <>
struct PrimitiveConversions::Translate<ZMarkTerminateState> : public std::true_type {
  typedef ZMarkTerminateState Value;
  typedef uint64_t            Decayed;

  static Decayed decay(Value v) {
    return v._state;
  }

  static Value recover(Decayed d) {
    ZMarkTerminateState state;
    state._state = d;
    return state;
  }
};

#endif // SHARE_GC_Z_ZMARKTERMINATESTATE_HPP

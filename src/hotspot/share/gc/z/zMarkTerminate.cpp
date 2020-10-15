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

#include "precompiled.hpp"
#include "gc/z/zMarkStack.inline.hpp"
#include "gc/z/zMarkTerminate.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"

void ZMarkTerminate::reset(uint nworkers) {
  _state = ZMarkTerminateState(nworkers, 0 /* active_stripes */);
}

void ZMarkTerminate::set_active_stripes(ZMarkStripeMap stripe_map) {
  ZMarkTerminateState old_state = Atomic::load_acquire(&_state);

  for (;;) {
    const uint32_t old_active_stripes = old_state.active_stripes();
    const uint32_t new_active_stripes = old_active_stripes | stripe_map.get();
    if (new_active_stripes == old_active_stripes) {
      // Already set
      return;
    }

    const uint old_nactive_workers = old_state.nactive_workers();
    const ZMarkTerminateState new_state(old_nactive_workers, new_active_stripes);
    const ZMarkTerminateState prev_state = Atomic::cmpxchg(&_state, old_state, new_state);
    if (prev_state == old_state) {
      // Success
      return;
    }

    // Retry
    old_state = prev_state;
  }
}

bool ZMarkTerminate::has_active_stripes() const {
  const ZMarkTerminateState state = Atomic::load_acquire(&_state);
  return state.active_stripes() != 0;
}

bool ZMarkTerminate::enter_idle_mode(ZMarkStripeMap stripe_map) {
  // If the selected stripe flags are cleared, decrement number of active
  // workers and enter idle mode. Otherwise, clear the stripe flags and
  // don't enter idle mode.

  ZMarkTerminateState old_state = Atomic::load_acquire(&_state);

  for (;;) {
    assert(!old_state.is_cleared(), "Invalid state");
    const uint32_t old_active_stripes = old_state.active_stripes();
    const uint32_t new_active_stripes = old_active_stripes & ~stripe_map.get();
    const bool should_idle = (new_active_stripes == old_active_stripes);
    const uint old_nactive_workers = old_state.nactive_workers();
    const uint new_nactive_workers = should_idle ? old_nactive_workers - 1 : old_nactive_workers;
    const ZMarkTerminateState new_state(new_nactive_workers, new_active_stripes);
    const ZMarkTerminateState prev_state = Atomic::cmpxchg(&_state, old_state, new_state);
    if (prev_state == old_state) {
      // Success
      return should_idle;
    }

    // Retry
    old_state = prev_state;
  }
}

bool ZMarkTerminate::exit_idle_mode(ZMarkStripeMap stripe_map) {
  // If the selected stripe flags are cleared, or if workers are terminating,
  // then we don't exit idle mode. Otherwise, increment number of active workers
  // and exit idle mode. We keep the stripe flags set to allow other worker to
  // also notice that there is work available. The stripe flags will be cleared
  // when worker on these stripes enter idle mode again.

  ZMarkTerminateState old_state = Atomic::load_acquire(&_state);

  for (;;) {
    const uint32_t old_active_stripes = old_state.active_stripes();
    if ((old_active_stripes & stripe_map.get()) == 0) {
      // Stripe flags cleared
      return false;
    }

    const uint old_nactive_workers = old_state.nactive_workers();
    if (old_nactive_workers == _terminate) {
      // Workers are terminating
      return false;
    }

    assert(!old_state.is_cleared(), "Invalid state");
    const uint new_nactive_workers = old_nactive_workers + 1;
    const ZMarkTerminateState new_state(new_nactive_workers, old_active_stripes);
    const ZMarkTerminateState prev_state = Atomic::cmpxchg(&_state, old_state, new_state);
    if (prev_state == old_state) {
      // Success
      return true;
    }

    // Retry
    old_state = prev_state;
  }
}

bool ZMarkTerminate::enter_terminate_mode() {
  // If all stripe flags are cleared, and no workers are active, then enter
  // terminate mode. Otherwise, remain in idle mode. Terminate mode sets the
  // number of workers to -1. This prevents other workers from exiting idle
  // mode and allow them to terminate.

  ZMarkTerminateState old_state = Atomic::load_acquire(&_state);

  for (;;) {
    const uint old_nactive_workers = old_state.nactive_workers();
    if (old_nactive_workers == _terminate) {
      // Terminate
      return true;
    }

    const uint32_t old_active_stripes = old_state.active_stripes();
    if (old_nactive_workers != 0 || old_active_stripes != 0) {
      // More work is available or some worker is still active
      return false;
    }

    const ZMarkTerminateState new_state(_terminate, 0 /* active_stripes */);
    const ZMarkTerminateState prev_state = Atomic::cmpxchg(&_state, old_state, new_state);
    if (prev_state == old_state) {
      // Success
      return true;
    }

    // Retry
    old_state = prev_state;
  }
}

bool ZMarkTerminate::idle(ZMarkStripeMap stripe_map) {
  if (!enter_idle_mode(stripe_map)) {
    // Don't idle, continue marking
    return false;
  }

  for (;;) {
    if (enter_terminate_mode()) {
      // Don't idle, terminate
      return true;
    }

    if (exit_idle_mode(stripe_map)) {
      // Don't idle, continue working
      return false;
    }

    // Idle
    os::naked_short_sleep(1);
  }
}

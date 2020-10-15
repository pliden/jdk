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
#include "gc/z/zMarkAffinity.hpp"
#include "gc/z/zMarkStack.inline.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zThread.hpp"
#include "logging/log.hpp"

static bool should_enable_numa_affinity(ZMarkStripeSet* stripes, uint nworkers, bool steal_from_all) {
  if (steal_from_all) {
    // Disable, steal from all stripes
    return false;
  }

  if (!ZNUMA::is_enabled()) {
    // Disable, NUMA support not enabled
    return false;
  }

  if (ZNUMA::count() > stripes->nstripes()) {
    // Disable, less than one stripe per node
    return false;
  }

  if (ZNUMA::count() * 4 > nworkers) {
    // Disable, less than 4 workers per node
    return false;
  }

  // Enable
  return true;
}

void ZMarkAffinity::set_default_affinity(ZMarkStripeSet* stripes, uint nworkers, uint worker_id) {
  // Select stripe
  const size_t nstripes = stripes->nstripes();
  const size_t spillover_limit = (nworkers / nstripes) * nstripes;
  size_t index;

  if (worker_id < spillover_limit) {
    // Not a spillover worker, use natural stripe
    index = worker_id & (nstripes - 1);
  } else {
    // Distribute spillover workers evenly across stripes
    const size_t spillover_nworkers = nworkers - spillover_limit;
    const size_t spillover_worker_id = worker_id - spillover_limit;
    const double spillover_chunk = (double)nstripes / (double)spillover_nworkers;
    index = spillover_worker_id * spillover_chunk;
  }

  assert(index < nstripes, "Invalid index");
  _stripe = stripes->stripe_at(index);

  // Steal from all stripes
  for (size_t i = 0; i < nstripes; i++) {
    _stripe_map.set(i);
  }
}

#if 0
void ZMarkAffinity::set_numa_affinity(ZMarkStripeSet* stripes, uint nworkers, uint worker_id) {
  // Select stripe

  // Set thread affinity
  const size_t stripe_id = stripes->stripe_id(stripe);
  const uint32_t nodes = ZNUMA::count();
  const uint32_t node_id = stripe_id % nodes;

  // Set home stripe
  _stripe_map.set(stripe_id);

  // Set victim stripes
  ZMarkStripe* victim = stripes->stripe_next(stripe);
  while (victim != stripe) {
    const size_t victim_id = stripes->stripe_id(victim);
    if (victim_id % nodes == node_id) {
      _stripe_map.set(victim_id);
    }

    victim = stripes->stripe_next(victim);
  }

  ZNUMA::set_thread_affinity(node_id);
}

void ZMarkAffinity::clear_numa_affinity() {
  ZNUMA::clear_thread_affinity();
}
#endif

ZMarkAffinity::ZMarkAffinity(ZMarkStripeSet* stripes, uint nworkers, uint worker_id, bool steal_from_all) :
    _stripe(NULL),
    _stripe_map(),
    _numa_affinity(should_enable_numa_affinity(stripes, nworkers, steal_from_all)) {
#if 0
  if (_numa_affinity) {
    set_numa_affinity(stripes, nworkers, worker_id);
  } else {
    set_default_affinity(stripes, nworkers, worker_id);
  }

  print(stripes, stripe);
#else
  set_default_affinity(stripes, nworkers, worker_id);
#endif
}

ZMarkAffinity::~ZMarkAffinity() {
#if 0
  if (_numa_affinity) {
    clear_numa_affinity();
  }
#endif
}

ZMarkStripe* ZMarkAffinity::home_stripe() const {
  return _stripe;
}

ZMarkStripeMap ZMarkAffinity::stripe_map() const {
  return _stripe_map;
}

#if 0
void ZMarkAffinity::print(ZMarkStripeSet* stripes) const {
  LogTarget(Info, gc, marking) log;
  if (!log.is_enabled()) {
    return;
  }

  const size_t nstripes = stripes->nstripes();
  const size_t stripe_id = stripes->stripe_id(stripe);
  const uint32_t nodes = ZNUMA::count();
  const uint32_t node_id = stripe_id % nodes;
  char map[128] = {};

  for (size_t id = 0; id < nstripes; id++) {
    if (_stripe_map.get(id)) {
      if (id == stripe_id) {
        // Home stripe
        map[id] = 'H';
      } else {
        // Steal stripe
        map[id] = 'S';
      }
    } else {
      map[id] = '.';
    }
  }

  log.print("Mark Affinity (%s): %s, Node: %u(%u), Stripes: %s",
            ZThread::name(),
            _enabled ? "Enabled" : "Disabled",
            node_id,
            nodes,
            map);
}
#endif

/*
 * Copyright (c) 2015, 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMARK_HPP
#define SHARE_GC_Z_ZMARK_HPP

#include "gc/z/zMarkFlush.hpp"
#include "gc/z/zMarkStack.hpp"
#include "gc/z/zMarkStackAllocator.hpp"
#include "gc/z/zMarkStackEntry.hpp"
#include "gc/z/zMarkTerminate.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"

class Thread;
class ZMarkCache;
class ZPageTable;
class ZWorkers;

class ZMark {
  template <typename Context> friend class ZMarkTask;

private:
  ZWorkers* const     _workers;
  ZPageTable* const   _page_table;
  ZMarkStackAllocator _allocator;
  ZMarkStripeSet      _stripes;
  ZMarkFlush          _flush;
  ZMarkTerminate      _terminate;
  uint32_t            _nrestart;
  uint32_t            _ncomplete;
  uint32_t            _ncontinue;
  uint                _nworkers;

  size_t calculate_nstripes(uint nworkers) const;
  ZMarkStripeMap calculate_stripe_map(ZMarkStripe* stripe, size_t nvictims);

  void prepare_mark();

  bool is_array(uintptr_t addr) const;
  void push_partial_array(uintptr_t addr, size_t size, bool finalizable);
  void follow_small_array(uintptr_t addr, size_t size, bool finalizable);
  void follow_large_array(uintptr_t addr, size_t size, bool finalizable);
  void follow_array(uintptr_t addr, size_t size, bool finalizable);
  void follow_partial_array(ZMarkStackEntry entry, bool finalizable);
  void follow_array_object(objArrayOop obj, bool finalizable);
  void follow_object(oop obj, bool finalizable);
  bool try_mark_object(ZMarkCache* cache, uintptr_t addr, bool finalizable);
  void mark_and_follow(ZMarkCache* cache, ZMarkStackEntry entry);

  void publish(ZMarkThreadLocalStacks* stacks);
  void free(ZMarkThreadLocalStacks* stacks);

  template <typename Context>
  bool drain(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkCache* cache, Context* context);

  template <typename Context>
  bool drain_and_publish(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkCache* cache, Context* context);

  bool steal(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkStripeMap stripe_map);
  bool idle(ZMarkStripeMap stripe_map);

  void reset(uint nworkers);

  template <typename Context>
  void work();

  bool restart();
  bool complete();

  void verify_termination() const;
  void verify_thread_stacks_empty() const;
  void verify_stripe_stacks_empty() const;
  void verify_all_stacks_empty() const;

public:
  ZMark(ZWorkers* workers, ZPageTable* page_table);

  bool is_initialized() const;

  template <bool follow, bool finalizable, bool publish>
  void mark_object(uintptr_t addr);

  void start();
  void mark(bool initial);
  bool end();

  void flush(Thread* thread, bool free_remaining);
};

#endif // SHARE_GC_Z_ZMARK_HPP

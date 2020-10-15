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

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zMark.inline.hpp"
#include "gc/z/zMarkAffinity.hpp"
#include "gc/z/zMarkCache.inline.hpp"
#include "gc/z/zMarkContext.inline.hpp"
#include "gc/z/zMarkStack.inline.hpp"
#include "gc/z/zOopClosures.inline.hpp"
#include "gc/z/zPage.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zThread.inline.hpp"
#include "gc/z/zThreadLocalAllocBuffer.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "gc/z/zWorkers.inline.hpp"
#include "logging/log.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/stackWatermark.hpp"
#include "runtime/stackWatermarkSet.inline.hpp"
#include "runtime/task.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"

static const ZStatSubPhase ZSubPhaseConcurrentMarkFlushRestart("Concurrent Mark Flush Restart");
static const ZStatSubPhase ZSubPhasePauseMarkEndComplete("Pause Mark End Complete");

ZMark::ZMark(ZWorkers* workers, ZPageTable* page_table) :
    _workers(workers),
    _page_table(page_table),
    _allocator(),
    _stripes(),
    _flush(this),
    _terminate(),
    _nrestart(0),
    _ncomplete(0),
    _ncontinue(0),
    _nworkers(0) {}

bool ZMark::is_initialized() const {
  return _allocator.is_initialized();
}

size_t ZMark::calculate_nstripes(uint nworkers) const {
  // Calculate the number of stripes from the number of workers we use,
  // where the number of stripes must be a power of two and we want to
  // have at least one worker per stripe.
  const size_t nstripes = round_down_power_of_2(nworkers);
  return MIN2(nstripes, ZMarkStripesMax);
}

ZMarkStripeMap ZMark::calculate_stripe_map(ZMarkStripe* stripe, size_t nvictims) {
  ZMarkStripeMap map;

  // Set home stripe
  map.set(_stripes.stripe_id(stripe));

  // Set steal victim stripes
  const size_t nvictims_capped = MIN2(nvictims, _stripes.nstripes() - 1);
  for (size_t i = 0; i < nvictims_capped; i++) {
    stripe = _stripes.stripe_next(stripe);
    map.set(_stripes.stripe_id(stripe));
  }

  return map;
}

void ZMark::prepare_mark() {
  // Increment global sequence number to invalidate
  // marking information for all pages.
  ZGlobalSeqNum++;

  // Reset restart/complete/continue counters
  _nrestart = _ncomplete = _ncontinue = 0;

  // Set number of workers to use
  _nworkers = _workers->nconcurrent();

  // Set number of mark stripes to use, based on number
  // of workers we will use in the concurrent mark phase.
  const size_t nstripes = calculate_nstripes(_nworkers);
  _stripes.set_nstripes(nstripes);

  // Update statistics
  ZStatMark::set_at_mark_start(nstripes);

  // Print worker/stripe distribution
  LogTarget(Debug, gc, marking) log;
  if (log.is_enabled()) {
    log.print("Mark Worker/Stripe Distribution");
    for (uint worker_id = 0; worker_id < _nworkers; worker_id++) {
      const ZMarkStripe* const stripe = _stripes.stripe_for_worker(_nworkers, worker_id);
      const size_t stripe_id = _stripes.stripe_id(stripe);
      log.print("  Worker %u(%u) -> Stripe " SIZE_FORMAT "(" SIZE_FORMAT ")",
                worker_id, _nworkers, stripe_id, nstripes);
    }
  }
}

void ZMark::start() {
  // Verification
  if (ZVerifyMarking) {
    verify_all_stacks_empty();
  }

  // Prepare for concurrent mark
  prepare_mark();
}

void ZMark::reset(uint nworkers) {
  // Set number of active workers
  _terminate.reset(nworkers);
}

bool ZMark::is_array(uintptr_t addr) const {
  return ZOop::from_address(addr)->is_objArray();
}

void ZMark::push_partial_array(uintptr_t addr, size_t size, bool finalizable) {
  assert(is_aligned(addr, ZMarkPartialArrayMinSize), "Address misaligned");
  ZMarkThreadLocalStacks* const stacks = ZThreadLocalData::stacks(Thread::current());
  ZMarkStripe* const stripe = _stripes.stripe_for_addr(addr);
  const uintptr_t offset = ZAddress::offset(addr) >> ZMarkPartialArrayMinSizeShift;
  const uintptr_t length = size / oopSize;
  const ZMarkStackEntry entry(offset, length, finalizable);

  log_develop_trace(gc, marking)("Array push partial: " PTR_FORMAT " (" SIZE_FORMAT "), stripe: " SIZE_FORMAT,
                                 addr, size, _stripes.stripe_id(stripe));

  stacks->push(&_allocator, &_stripes, stripe, entry, false /* publish */);
}

void ZMark::follow_small_array(uintptr_t addr, size_t size, bool finalizable) {
  assert(size <= ZMarkPartialArrayMinSize, "Too large, should be split");
  const size_t length = size / oopSize;

  log_develop_trace(gc, marking)("Array follow small: " PTR_FORMAT " (" SIZE_FORMAT ")", addr, size);

  ZBarrier::mark_barrier_on_oop_array((oop*)addr, length, finalizable);
}

void ZMark::follow_large_array(uintptr_t addr, size_t size, bool finalizable) {
  assert(size <= (size_t)arrayOopDesc::max_array_length(T_OBJECT) * oopSize, "Too large");
  assert(size > ZMarkPartialArrayMinSize, "Too small, should not be split");
  const uintptr_t start = addr;
  const uintptr_t end = start + size;

  // Calculate the aligned middle start/end/size, where the middle start
  // should always be greater than the start (hence the +1 below) to make
  // sure we always do some follow work, not just split the array into pieces.
  const uintptr_t middle_start = align_up(start + 1, ZMarkPartialArrayMinSize);
  const size_t    middle_size = align_down(end - middle_start, ZMarkPartialArrayMinSize);
  const uintptr_t middle_end = middle_start + middle_size;

  log_develop_trace(gc, marking)("Array follow large: " PTR_FORMAT "-" PTR_FORMAT" (" SIZE_FORMAT "), "
                                 "middle: " PTR_FORMAT "-" PTR_FORMAT " (" SIZE_FORMAT ")",
                                 start, end, size, middle_start, middle_end, middle_size);

  // Push unaligned trailing part
  if (end > middle_end) {
    const uintptr_t trailing_addr = middle_end;
    const size_t trailing_size = end - middle_end;
    push_partial_array(trailing_addr, trailing_size, finalizable);
  }

  // Push aligned middle part(s)
  uintptr_t partial_addr = middle_end;
  while (partial_addr > middle_start) {
    const size_t parts = 2;
    const size_t partial_size = align_up((partial_addr - middle_start) / parts, ZMarkPartialArrayMinSize);
    partial_addr -= partial_size;
    push_partial_array(partial_addr, partial_size, finalizable);
  }

  // Follow leading part
  assert(start < middle_start, "Miscalculated middle start");
  const uintptr_t leading_addr = start;
  const size_t leading_size = middle_start - start;
  follow_small_array(leading_addr, leading_size, finalizable);
}

void ZMark::follow_array(uintptr_t addr, size_t size, bool finalizable) {
  if (size <= ZMarkPartialArrayMinSize) {
    follow_small_array(addr, size, finalizable);
  } else {
    follow_large_array(addr, size, finalizable);
  }
}

void ZMark::follow_partial_array(ZMarkStackEntry entry, bool finalizable) {
  const uintptr_t addr = ZAddress::good(entry.partial_array_offset() << ZMarkPartialArrayMinSizeShift);
  const size_t size = entry.partial_array_length() * oopSize;

  follow_array(addr, size, finalizable);
}

void ZMark::follow_array_object(objArrayOop obj, bool finalizable) {
  if (finalizable) {
    ZMarkBarrierOopClosure<true /* finalizable */> cl;
    cl.do_klass(obj->klass());
  } else {
    ZMarkBarrierOopClosure<false /* finalizable */> cl;
    cl.do_klass(obj->klass());
  }

  const uintptr_t addr = (uintptr_t)obj->base();
  const size_t size = (size_t)obj->length() * oopSize;

  follow_array(addr, size, finalizable);
}

void ZMark::follow_object(oop obj, bool finalizable) {
  if (finalizable) {
    ZMarkBarrierOopClosure<true /* finalizable */> cl;
    obj->oop_iterate(&cl);
  } else {
    ZMarkBarrierOopClosure<false /* finalizable */> cl;
    obj->oop_iterate(&cl);
  }
}

bool ZMark::try_mark_object(ZMarkCache* cache, uintptr_t addr, bool finalizable) {
  ZPage* const page = _page_table->get(addr);
  if (page->is_allocating()) {
    // Newly allocated objects are implicitly marked
    return false;
  }

  // Try mark object
  bool inc_live = false;
  const bool success = page->mark_object(addr, finalizable, inc_live);
  if (inc_live) {
    // Update live objects/bytes for page. We use the aligned object
    // size since that is the actual number of bytes used on the page
    // and alignment paddings can never be reclaimed.
    const size_t size = ZUtils::object_size(addr);
    const size_t aligned_size = align_up(size, page->object_alignment());
    cache->inc_live(page, aligned_size);
  }

  return success;
}

void ZMark::mark_and_follow(ZMarkCache* cache, ZMarkStackEntry entry) {
  // Decode flags
  const bool finalizable = entry.finalizable();
  const bool partial_array = entry.partial_array();

  if (partial_array) {
    follow_partial_array(entry, finalizable);
    return;
  }

  // Decode object address and follow flag
  const uintptr_t addr = entry.object_address();

  if (!try_mark_object(cache, addr, finalizable)) {
    // Already marked
    return;
  }

  if (is_array(addr)) {
    // Decode follow flag
    const bool follow = entry.follow();

    // The follow flag is currently only relevant for object arrays
    if (follow) {
      follow_array_object(objArrayOop(ZOop::from_address(addr)), finalizable);
    }
  } else {
    follow_object(ZOop::from_address(addr), finalizable);
  }
}

template <typename Context>
bool ZMark::drain(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkCache* cache, Context* context) {
  ZMarkStackEntry entry;

  // Drain stripe stacks
  while (stacks->pop(&_allocator, &_stripes, stripe, entry)) {
    mark_and_follow(cache, entry);

    // Check timeout
    if (context->should_timeout()) {
      // Timeout
      return false;
    }
  }

  // Success
  return true;
}

void ZMark::publish(ZMarkThreadLocalStacks* stacks) {
  // Flush stacks
  const ZMarkStripeMap published = stacks->flush(&_allocator, &_stripes);

  // Signal to workers that more work is available
  _terminate.set_active_stripes(published);
}

void ZMark::free(ZMarkThreadLocalStacks* stacks) {
  // Make sure all stacks have been published
  assert(stacks->is_empty(), "Should be empty");

  // Free remaining stacks
  stacks->free(&_allocator);
}

template <typename Context>
bool ZMark::drain_and_publish(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkCache* cache, Context* context) {
  // Drain stripe
  const bool success = drain(stripe, stacks, cache, context);

  // Publish stacks
  publish(stacks);

  return success;
}

bool ZMark::steal(ZMarkStripe* stripe, ZMarkThreadLocalStacks* stacks, ZMarkStripeMap map) {
  // Try to steal a stack from another stripe in the stripe map
  ZMarkStripe* victim = _stripes.stripe_next(stripe);
  while (victim != stripe) {
    const size_t victim_id = _stripes.stripe_id(victim);
    if (map.get(victim_id)) {
      ZMarkStack* const stack = victim->steal_stack();
      if (stack != NULL) {
        // Success, install the stolen stack
        stacks->install(&_stripes, stripe, stack);
        return true;
      }
    }

    // Failed, try next stripe
    victim = _stripes.stripe_next(victim);
  }

  // Nothing to steal
  return false;
}

bool ZMark::idle(ZMarkStripeMap map) {
  return _terminate.idle(map);
}

template <typename Context>
void ZMark::work() {
  Context context;
  //ZMarkStripe* const stripe = _stripes.stripe_for_worker(_nworkers, ZThread::worker_id());
  ZMarkCache cache(_stripes.nstripes());
  ZMarkAffinity const affinity(&_stripes, _nworkers, ZThread::worker_id(), context.steal_from_all_stripes());
  ZMarkStripe* const stripe = affinity.home_stripe();
  ZMarkStripeMap const map = affinity.stripe_map();
  ZMarkThreadLocalStacks* const stacks = ZThreadLocalData::stacks(Thread::current());

  for (;;) {
    if (!drain_and_publish(stripe, stacks, &cache, &context)) {
      // Timed out
      break;
    }

    if (steal(stripe, stacks, map)) {
      // Stole work
      continue;
    }

    if (idle(map)) {
      // Terminate
      break;
    }
  }

  // Free remaining stacks
  free(stacks);
}

class ZMarkConcurrentRootsIteratorClosure : public ZRootsIteratorClosure {
public:
  ZMarkConcurrentRootsIteratorClosure() {
    ZThreadLocalAllocBuffer::reset_statistics();
  }

  ~ZMarkConcurrentRootsIteratorClosure() {
    ZThreadLocalAllocBuffer::publish_statistics();
  }

  virtual bool should_disarm_nmethods() const {
    return true;
  }

  virtual void do_thread(Thread* thread) {
    JavaThread* const jt = thread->as_Java_thread();
    StackWatermarkSet::finish_processing(jt, this, StackWatermarkKind::gc);
    ZThreadLocalAllocBuffer::update_stats(jt);
  }

  virtual void do_oop(oop* p) {
    ZBarrier::mark_barrier_on_oop_field(p, false /* finalizable */);
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }
};

class ZMarkConcurrentRootsTask : public ZTask {
private:
  ZMark* const                        _mark;
  SuspendibleThreadSetJoiner          _sts_joiner;
  ZConcurrentRootsIteratorClaimStrong _roots;
  ZMarkConcurrentRootsIteratorClosure _cl;

public:
  ZMarkConcurrentRootsTask(ZMark* mark) :
      ZTask("ZMarkConcurrentRootsTask"),
      _mark(mark),
      _sts_joiner(),
      _roots(),
      _cl() {
    ClassLoaderDataGraph_lock->lock();
  }

  ~ZMarkConcurrentRootsTask() {
    ClassLoaderDataGraph_lock->unlock();
  }

  virtual void work() {
    _roots.oops_do(&_cl);

    // Flush and free worker stacks. Needed here since the set of
    // workers executing during root scanning can be different from
    // the set of workers executing during mark.
    _mark->flush(Thread::current(), true /* free_magazine */);
  }
};

template <typename Context>
class ZMarkTask : public ZTask {
private:
  ZMark* const _mark;

public:
  ZMarkTask(ZMark* mark, uint nworkers) :
      ZTask("ZMarkTask"),
      _mark(mark) {
    _mark->reset(nworkers);
  }

  virtual void work() {
    _mark->work<Context>();
  }
};

bool ZMark::restart() {
  // Restart a limited number of times
  const uint32_t max = ZMarkRestartMax * (_ncontinue + 1);
  if (_nrestart == max) {
    return false;
  }

  // Flush VM and Java threads
  ZStatTimer timer(ZSubPhaseConcurrentMarkFlushRestart);
  _flush.vm_and_java_threads();

  // Restart marking if there are active stripes
  if (_terminate.has_active_stripes()) {
    _nrestart++;
    return true;
  }

  return false;
}

void ZMark::mark(bool initial) {
  if (initial) {
    ZMarkConcurrentRootsTask task(this);
    _workers->run_concurrent(&task);
  }

  do {
    ZMarkFlushPeriodic flush(this);
    ZMarkTask<ZMarkContext> task(this, _workers->nconcurrent());
    _workers->run_concurrent(&task);
  } while (restart());
}

bool ZMark::complete() {
  // Verification
  if (ZVerifyMarking) {
    verify_termination();
  }

  // Flush all threads
  _flush.all_threads();

  // Verification
  if (ZVerifyMarking) {
    verify_termination();
  }

  if (_terminate.has_active_stripes()) {
    // More work available. Continue marking inside for a limited
    // about of time. We mark using a single thread to avoid the
    // cost of starting and stopping worker threads, which could
    // otherwise consume a considerable amount of our time budget.
    ZStatTimer timer(ZSubPhasePauseMarkEndComplete);
    ZMarkTask<ZMarkEndContext> task(this, 1 /* nworkers */);
    _workers->run_serial(&task);
    _ncomplete++;
  }

  // Verification
  if (ZVerifyMarking) {
    verify_termination();
  }

  // Marking is complete if there are no active stripes
  return !_terminate.has_active_stripes();
}

bool ZMark::end() {
  // Try complete marking
  if (!complete()) {
    // Continue concurrent mark
    _ncontinue++;
    return false;
  }

  // Verification
  if (ZVerifyMarking) {
    verify_all_stacks_empty();
  }

  // Update statistics
  ZStatMark::set_at_mark_end(_nrestart, _ncomplete, _ncontinue);

  // Mark completed
  return true;
}

void ZMark::flush(Thread* thread, bool free_magazine) {
  ZMarkThreadLocalStacks* const stacks = ZThreadLocalData::stacks(thread);

  publish(stacks);

  if (free_magazine) {
    free(stacks);
  }
}

class ZVerifyMarkStacksEmptyClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    ZMarkThreadLocalStacks* const stacks = ZThreadLocalData::stacks(thread);
    guarantee(stacks->is_empty(), "Should be empty");
    guarantee(stacks->is_freed(), "Should be freed");
  }
};

void ZMark::verify_all_stacks_empty() const {
  // Verify all thread stacks empty
  ZVerifyMarkStacksEmptyClosure cl;
  Threads::threads_do(&cl);

  // Verify all stripes empty
  guarantee(_stripes.is_empty(), "Should be empty");
}

void ZMark::verify_termination() const {
  guarantee(_terminate.has_active_stripes() != _stripes.is_empty(), "Termination state mismatch");
}

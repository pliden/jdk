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
#include "gc/z/zMark.hpp"
#include "gc/z/zMarkFlush.hpp"
#include "gc/z/zStat.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/debug.hpp"

static const ZStatSubPhase ZSubPhaseConcurrentMarkFlush("Concurrent Mark Flush Periodic");

class VM_ZMarkFlush : public VM_Operation {
private:
  ZMarkFlushClosure* const _cl;

public:
  VM_ZMarkFlush(ZMarkFlushClosure* cl) :
      _cl(cl) {}

  virtual VMOp_Type type() const {
    return VMOp_ZMarkFlush;
  }

  virtual bool evaluate_at_safepoint() const {
    // Do not safepoint, only flushing the VM thread
    return false;
  }

  virtual void doit() {
    _cl->do_thread(Thread::current());
  }
};

static void flush_vm_and_java_threads(ZMarkFlushClosure* cl) {
  VM_ZMarkFlush op(cl);
  VMThread::execute(&op);
  Handshake::execute(cl);
}

static void flush_all_threads(ZMarkFlushClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  Threads::threads_do(cl);
}

ZMarkFlushClosure::ZMarkFlushClosure(ZMark* mark, bool free_magazine) :
    HandshakeClosure("ZMarkFlush"),
    _mark(mark),
    _free_magazine(free_magazine) {}

void ZMarkFlushClosure::do_thread(Thread* thread) {
  _mark->flush(thread, _free_magazine);
}

ZMarkFlushPeriodicTask::ZMarkFlushPeriodicTask(ZMark* mark) :
    PeriodicTask(ZMarkFlushInterval),
    _mark(mark) {}

void ZMarkFlushPeriodicTask::task() {
  ZStatTimer timer(ZSubPhaseConcurrentMarkFlush);
  ZMarkFlushClosure cl(_mark, false /* free_magazine */);
  flush_vm_and_java_threads(&cl);
}

ZMarkFlushPeriodic::ZMarkFlushPeriodic(ZMark* mark) :
    _task(mark) {
  _task.enroll();
}

ZMarkFlushPeriodic::~ZMarkFlushPeriodic() {
  _task.disenroll();
}

ZMarkFlush::ZMarkFlush(ZMark* mark) :
    _mark(mark) {}

void ZMarkFlush::vm_and_java_threads() {
  ZMarkFlushClosure cl(_mark, true /* free_magazine */);
  flush_vm_and_java_threads(&cl);
}

void ZMarkFlush::all_threads() {
  ZMarkFlushClosure cl(_mark, true /* free_magazine */);
  flush_all_threads(&cl);
}

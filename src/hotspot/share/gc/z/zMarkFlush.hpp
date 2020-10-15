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

#ifndef SHARE_GC_Z_ZMARKFLUSH_HPP
#define SHARE_GC_Z_ZMARKFLUSH_HPP

#include "runtime/handshake.hpp"
#include "runtime/task.hpp"

class ZMark;

class ZMarkFlushClosure : public HandshakeClosure {
private:
  ZMark* const _mark;
  const bool   _free_remaining;

public:
  ZMarkFlushClosure(ZMark* mark, bool free_remaining);

  virtual void do_thread(Thread* thread);
};

class ZMarkFlushPeriodicTask : public PeriodicTask {
private:
  ZMark* const _mark;

public:
  ZMarkFlushPeriodicTask(ZMark* mark);

  virtual void task();
};

class ZMarkFlushPeriodic {
private:
  ZMarkFlushPeriodicTask _task;

public:
  ZMarkFlushPeriodic(ZMark* mark);
  ~ZMarkFlushPeriodic();
};

class ZMarkFlush {
private:
  ZMark* const _mark;

public:
  ZMarkFlush(ZMark* mark);

  void vm_and_java_threads();
  void all_threads();
};

#endif // SHARE_GC_Z_ZMARKFLUSH_HPP

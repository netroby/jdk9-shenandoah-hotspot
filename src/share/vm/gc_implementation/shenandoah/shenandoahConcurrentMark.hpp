/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"
#include "gc_implementation/shenandoah/sharedOverflowMarkQueue.hpp"

typedef Padded<OopTaskQueue> SCMObjToScanQueue;
typedef GenericTaskQueueSet<SCMObjToScanQueue, mtGC> SCMObjToScanQueueSet;

class ShenandoahConcurrentMark: public CHeapObj<mtGC> {

private:
  // The per-worker-thread work queues
  SCMObjToScanQueueSet* _task_queues;

  // The shared mark stack that is used in case of overflow.
  SharedOverflowMarkQueue* _overflow_queue;

  bool                    _aborted;       

public:
  //  ShenandoahConcurrentMark();

  void scanRootRegions();
  void markFromRoots();
  void checkpointRootsFinal();
  void finishMarkFromRoots();
  void drain_satb_buffers();
  bool has_aborted() {return _aborted;}
  SharedOverflowMarkQueue* overflow_queue();

  void addTask(oop obj, int worker_id);
  //  oop popTask(int worker_id);

  uint _max_worker_id;
  ParallelTaskTerminator* _terminator;
  ParallelTaskTerminator* terminator() { return _terminator;}
  SCMObjToScanQueueSet* task_queues() { return _task_queues;}

  // We need to do this later when the heap is already created.
  void initialize(FlexibleWorkGang* workers);

#if TASKQUEUE_STATS
  static void print_taskqueue_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_taskqueue_stats(outputStream* const st = gclog_or_tty) const;
  void reset_taskqueue_stats();
#endif // TASKQUEUE_STATS

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

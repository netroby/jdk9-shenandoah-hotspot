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

class SCMTask; 

typedef GenericTaskQueue<oop, mtGC>            SCMTaskQueue;
typedef GenericTaskQueueSet<SCMTaskQueue, mtGC> SCMTaskQueueSet;


class ShenandoahConcurrentMark: public CHeapObj<mtGC> {
private:
  uint                    _max_worker_id; // maximum worker id
  uint                    _active_tasks;  // task num currently active
  SCMTask**                _tasks;         // task queue array (max_worker_id len)
  SCMTaskQueueSet*         _task_queues;   // task queue set
  ParallelTaskTerminator  _terminator;    // for termination
  bool                    _aborted;       
  uint epoch;

public:
  ShenandoahConcurrentMark();
  void scanRootRegions();
  void markFromRoots();
  void checkpointRootsFinal();
  void finishMarkFromRoots();
  bool has_aborted() {return _aborted;}

  void addTask(oop obj);
  void addTask(oop obj, int worker_id);
  oop popTask(int worker_id);
  void setEpoch(uint x) { epoch = x;}
  uint getEpoch() { return epoch;}

};


class SCMRootRegionScanTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;

public:
  SCMRootRegionScanTask(ShenandoahConcurrentMark* cm);

  void work(uint worker_id);
};

class SCMConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;

public:
  SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm);

  void work(uint worker_id);
};

class SCMTask : public TerminatorTerminator {

public:
  void do_marking_step();

};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

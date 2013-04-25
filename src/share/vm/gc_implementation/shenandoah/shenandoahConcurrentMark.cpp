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

#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"


SCMRootRegionScanTask::SCMRootRegionScanTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void SCMRootRegionScanTask::work(uint worker_id) {
}

// Fixme CHF should be merged with version in  shenandoahHeap.cpp
class ShenandoahMarkRefsClosure2 : public OopsInGenClosure {
  uint epoch;
public: 
  ShenandoahMarkRefsClosure2(uint e) : epoch(e) {}

  void do_oop_work(oop* p) {
    oop obj = *p;
    ShenandoahHeap* sh = (ShenandoahHeap* ) Universe::heap();
    if (obj != NULL && (sh->is_in(obj))) {
      sh->concurrentMark()->addTask(obj);
	if (obj->has_displaced_mark()) {
	  if (obj->displaced_mark()->age() != epoch) {
	    obj->set_displaced_mark(obj->displaced_mark()->set_age(epoch));
	  }
	} else {
	  if (obj->mark()->age() != epoch) {
	    obj->set_mark(obj->mark()->set_age(epoch));
	  }
	}
      }
  }

  void do_oop(narrowOop* p) {assert(false, "narrorOops not supported");}
  void do_oop(oop* p) {do_oop_work(p);}
};

SCMConcurrentMarkingTask::SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void SCMConcurrentMarkingTask::work(uint worker_id) {
  oop obj = _cm->popTask(worker_id);
  while (obj != NULL) {
    ShenandoahMarkRefsClosure2 cl(_cm->getEpoch());
    obj->oop_iterate(&cl);
    obj = _cm->popTask(worker_id);
  }
}

// ShenandoahConcurrentMark::ShenandoahConcurrentMark() :
//   _max_worker_id(MAX2((uint)ParallelGCThreads, 1U)),
//   _task_queues(new SCMTaskQueueSet((int) _max_worker_id)),
//   _terminator(ParallelTaskTerminator((int) _max_worker_id, _task_queues))
// {
  
// }

void ShenandoahConcurrentMark::initialize(FlexibleWorkGang* workers) {
  uint _max_worker_id = MAX2((uint)ParallelGCThreads, 1U);
 _task_queues = new SCMObjToScanQueueSet((int) _max_worker_id);

  tty->print("ActiveWorkers  = %d total workers = %d\n", workers->active_workers(), 
	     workers->total_workers());

  for (uint i = 0; i < _max_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
}

void ShenandoahConcurrentMark::scanRootRegions() {
  SCMRootRegionScanTask task(this);
  task.work(0);
}

void ShenandoahConcurrentMark::markFromRoots() {
  tty->print_cr("Starting markFromRoots");
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  sh->start_concurrent_marking();
  SCMConcurrentMarkingTask* markingTask = new SCMConcurrentMarkingTask(this);
  sh->workers()->run_task(markingTask);
}

void ShenandoahConcurrentMark::finishMarkFromRoots() {
  SCMConcurrentMarkingTask markingTask(this);
  markingTask.work(0);
}

void ShenandoahConcurrentMark::checkpointRootsFinal() {
  SCMConcurrentMarkingTask markingTask(this);
  markingTask.work(0);
}



void ShenandoahConcurrentMark::addTask(oop obj, int q) {
  //  tty->print("addTask q = %d\n", q);
  if (obj->age() != epoch) {
    if (!_task_queues->queue(q)->push(obj)) {
      assert(false, "oops_overflowed_our_queues");
    }
  }
}


void ShenandoahConcurrentMark::addTask(oop obj) {
  addTask(obj, 0);
}

// This queue implementation is wonky
oop ShenandoahConcurrentMark::popTask(int q) {
  oop obj;
  bool result = _task_queues->queue(q)->pop_local(obj);
  //  tty->print"popTask: q = %d\n", q);
  if (result) 
    return obj;
  else {
    int seed = 17;
    result = _task_queues->steal(q, &seed, obj);
    if (result)
      return obj;
    else 
      return NULL;
  }
}

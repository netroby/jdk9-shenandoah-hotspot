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
#include "gc_implementation/shenandoah/shenandoahSATBQueue.hpp"

SCMRootRegionScanTask::SCMRootRegionScanTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void SCMRootRegionScanTask::work(uint worker_id) {
}

// We need to revisit this  CHF
// class SCMTerminatorTerminator : public TerminatorTerminator  { // So good we named it twice
//   bool should_exit_termination() {
//     return true;
//   }
// };

SCMConcurrentMarkingTask::SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm, 
						   ParallelTaskTerminator* terminator) :
  AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator) { }

void SCMConcurrentMarkingTask::work(uint worker_id) {
  int seed = 17;
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahMarkRefsClosure cl(sh->getEpoch(), worker_id);

  while (true) {
    oop obj;
    
    if (!_cm->task_queues()->queue(worker_id)->pop_local(obj)) {
      if (!_cm->task_queues()->steal(worker_id, &seed, obj)) {
        obj = _cm->overflow_queue()->pop();
        if (obj == NULL) {
          if (_terminator->offer_termination())
            break;  
          else 
            continue;
        }
      }
    }
    // We got one.
    
    assert(obj->is_oop(), "Oops, not an oop");
    // if (ShenandoahGCVerbose) {
    //   tty->print("popping object: "PTR_FORMAT"\n", obj);
    // }
    obj->oop_iterate(&cl);
  }
}

void ShenandoahConcurrentMark::initialize(FlexibleWorkGang* workers) {
  tty->print("ActiveWorkers  = %d total workers = %d\n", 
	     workers->active_workers(), 
	     workers->total_workers());
  _max_worker_id = MAX2((uint)ParallelGCThreads, 1U);
  _task_queues = new SCMObjToScanQueueSet((int) _max_worker_id);
  _overflow_queue = new SharedOverflowMarkQueue();

  for (uint i = 0; i < _max_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
  JavaThread::satb_mark_queue_set()->set_buffer_size(1014 /* G1SATBBufferSize */);
}

void ShenandoahConcurrentMark::scanRootRegions() {
  SCMRootRegionScanTask task(this);
  task.work(0);
}

void ShenandoahConcurrentMark::markFromRoots() {
  tty->print_cr("STOPPING THE WORLD: before marking");
  tty->print_cr("Starting markFromRoots");
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);

  
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator);
  sh->workers()->run_task(&markingTask);
  
  tty->print_cr("Finishing markFromRoots");
  tty->print_cr("RESUMING THE WORLD: after marking");
}

void ShenandoahConcurrentMark::finishMarkFromRoots() {
  tty->print_cr("Starting finishMarkFromRoots");
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  //  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);
  // Trace any (new) unmarked root references.
  sh->prepare_unmarked_root_objs();

  drain_satb_buffers();

  // TODO: I think we can parallelize this.
  //  SCMConcurrentMarkingTask markingTask(this, &terminator);
  //  markingTask.work(0);

  // For Now doing this sequentially.

  // First make sure queues other than 0 are empty.
  for (int i = 1; i < (int)_max_worker_id; i++)
    assert(_task_queues->queue(i)->is_empty(), "Only using task queue 0 for now");

  ShenandoahMarkRefsClosure cl(sh->getEpoch(), 0);
  oop obj;

  bool found = _task_queues->queue(0)->pop_local(obj);

  while (found) {
    if (ShenandoahGCVerbose) {
      tty->print("Pop single threaded Task: obj = "PTR_FORMAT"\n", obj);
    }
    assert(obj->is_oop(), "Oops, not an oop");
    obj->oop_iterate(&cl);
    found = _task_queues->queue(0)->pop_local(obj);
  }

  // Also drain our overflow queue.
  obj = _overflow_queue->pop();
  while (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    obj->oop_iterate(&cl);
    obj = _overflow_queue->pop();
  }

  assert(_task_queues->queue(0)->is_empty(), "Should be empty");
  tty->print_cr("Finishing finishMarkFromRoots");
#ifdef SLOWDEBUG
  for (int i = 0; i <(int)_max_worker_id; i++) {
    tty->print("Queue: %d:", i);
    _task_queues->queue(i)->stats.print(tty, 10);
    tty->print("\n");
    //    _task_queues->queue(i)->stats.verify();
  }
#endif
}

class DrainSATBClosure : public ShenandoahSATBElementClosure {

private:
  ShenandoahMarkObjsClosure* _mark_objs;

public:
  DrainSATBClosure(ShenandoahMarkObjsClosure* mark_objs) : _mark_objs(mark_objs) {}

  void do_satb_element(ShenandoahSATBElement* satb_element)  {
    oop prev_val = satb_element->get_previous_value();
    if (prev_val != NULL) {
      _mark_objs->do_object(prev_val);
    }
    ShenandoahHeap::heap()->maybe_update_oop_ref(satb_element->get_referrer());
  }
};

void ShenandoahConcurrentMark::drain_satb_buffers() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahMarkObjsClosure cl(sh->getEpoch(), 0);
  DrainSATBClosure drainCl(&cl);
  // This can be parallelized. See g1/concurrentMark.cpp
  ShenandoahSATBQueueSet* satb_mq_set = (ShenandoahSATBQueueSet*) JavaThread::satb_mark_queue_set();
  satb_mq_set->set_closure(&drainCl);
  while (satb_mq_set->apply_closure_to_completed_buffer());
  satb_mq_set->iterate_closure_all_threads();
  satb_mq_set->set_closure(NULL);

}

void ShenandoahConcurrentMark::checkpointRootsFinal() {
  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);
  SCMConcurrentMarkingTask markingTask(this, &terminator);
  markingTask.work(0);
}

int getAge(oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  assert(sh->is_in((HeapWord*) obj), "We are only interested in heap objects");
  if (obj->has_displaced_mark())
    return obj->displaced_mark()->age();
  else return obj->mark()->age();
}


void ShenandoahConcurrentMark::addTask(oop obj, int q) {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  int epoch = sh->getEpoch();
  int age = getAge(obj);

  assert(obj->is_oop(), "Oops, not an oop");

  // if (ShenandoahGCVerbose) {
  //   tty->print("addTask q = %d: obj = "PTR_FORMAT" epoch = %d object age = %d\n", q, obj, epoch, age);
  // }

  assert(age == epoch, "Only push marked objects on the queue");
  assert(sh->is_in((HeapWord*) obj), "Only push heap objects on the queue");

  if (!_task_queues->queue(q)->push(obj)) {
    tty->print_cr("WARNING: Shenandoah mark queues overflown, increase mark queue size -XX:MarkStackSize=VALUE");
    _overflow_queue->push(obj);
  }
}

SharedOverflowMarkQueue* ShenandoahConcurrentMark::overflow_queue() {
  return _overflow_queue;
}

// // This queue implementation is wonky
// oop ShenandoahConcurrentMark::popTask(int q) {
//   oop obj;

//   while (true) {
//     bool result = _task_queues->queue(q)->pop_local(obj);

//     if (result) {
//       tty->print("popTask:q = %d obj = "PTR_FORMAT"\n", q, obj);

//       ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
//       int epoch = sh->getEpoch();
//       int age = getAge(obj);

//       assert(age == epoch, "Only marked objects on the queue");

//       tty->print("popTask: q = %d obj = "PTR_FORMAT" epoch  = %d objects mark = %d\n", q, obj, epoch, age);

//       return obj;
//     } else {
//       int seed = 17;
//     result = _task_queues->steal(q, &seed, obj);
//     if (result) {
//       tty->print("stealTask:q = %d obj = "PTR_FORMAT"\n", q, obj);
//       return obj;
//     } else 
//       return NULL;
//   }
// }

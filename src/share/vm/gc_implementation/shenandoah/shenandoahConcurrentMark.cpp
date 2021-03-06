/*
  Copyright 2014 Red Hat, Inc. and/or its affiliates.
*/
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

#include "classfile/stringTable.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "memory/referenceProcessor.hpp"
#include "classfile/symbolTable.hpp"



// Mark the object and add it to the queue to be scanned
class ShenandoahMarkObjsClosure : public ObjectClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
  size_t* _live_data;
public: 
  ShenandoahMarkObjsClosure(uint worker_id) :
    _worker_id(worker_id),
    _heap((ShenandoahHeap*)(Universe::heap())),
    _live_data(NEW_C_HEAP_ARRAY(size_t, _heap->max_regions(), mtGC))
  {
    Copy::zero_to_bytes(_live_data, _heap->max_regions() * sizeof(size_t));
  }

  ~ShenandoahMarkObjsClosure()  {
    // Merge liveness data back into actual regions.

    // We need to lock the heap here, to avoid race with growing of heap.
    MutexLockerEx ml(ShenandoahHeap_lock, true);
    ShenandoahHeapRegion** regions = _heap->heap_regions();
    for (uint i = 0; i < _heap->num_regions(); i++) {
      regions[i]->increase_live_data(_live_data[i]);
    }
    FREE_C_HEAP_ARRAY(size_t, _live_data);
  }

  void do_object(oop obj) {
    ShenandoahConcurrentMark* scm = _heap->concurrentMark();

    if (obj != NULL) {

      assert(obj == oopDesc::bs()->resolve_oop(obj), "needs to be in to-space");

#ifdef ASSERT
      if (_heap->heap_region_containing(obj)->is_in_collection_set()) {
        tty->print_cr("trying to mark obj: "PTR_FORMAT" (%s) in dirty region: ", p2i((HeapWord*) obj), BOOL_TO_STR(_heap->isMarkedCurrent(obj)));
        //      _heap->heap_region_containing(obj)->print();
        //      _heap->print_heap_regions();
      }
#endif
      assert(_heap->cancelled_evacuation()
             || ! _heap->heap_region_containing(obj)->is_in_collection_set(),
             "we don't want to mark objects in from-space");
      assert(_heap->is_in(obj), "referenced objects must be in the heap. No?");
      if (_heap->mark_current(obj)) {
#ifdef ASSERT
        if (ShenandoahTraceConcurrentMarking) {
          tty->print_cr("marked obj: "PTR_FORMAT, p2i((HeapWord*) obj));
        }
#endif

        // Calculate liveness of heap region containing object.
        uint region_idx  = _heap->heap_region_index_containing(obj);
        _live_data[region_idx] += (obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE) * HeapWordSize;

        scm->add_task(obj, _worker_id);
      }
#ifdef ASSERT
      else {
        if (ShenandoahTraceConcurrentMarking) {
          tty->print_cr("failed to mark obj (already marked): "PTR_FORMAT, p2i((HeapWord*) obj));
        }
        assert(_heap->isMarkedCurrent(obj), "make sure object is marked");
      }
#endif

      /*
        else {
        tty->print_cr("already marked object "PTR_FORMAT", "INT32_FORMAT, p2i(obj), getMark(obj)->age());
        }
      */
    }
    /*
      else {
      if (obj != NULL) {
      tty->print_cr("not marking root object because it's not in heap: "PTR_FORMAT, p2i(obj));
      }
      }
    */

  }
};  


// Walks over all the objects in the generation updating any
// references to from space.

class ShenandoahMarkRefsClosure : public ExtendedOopClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
  ShenandoahMarkObjsClosure _mark_objs;

public: 
  ShenandoahMarkRefsClosure(uint worker_id) : 
    ExtendedOopClosure(((ShenandoahHeap *) Universe::heap())->ref_processor_cm()),
    _worker_id(worker_id),
    _heap((ShenandoahHeap*) Universe::heap()),
    _mark_objs(ShenandoahMarkObjsClosure(worker_id))
  {
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

  void do_oop(oop* p) {
    do_oop_work(p);
  }

private:
  void do_oop_work(oop* p) {
    // We piggy-back reference updating to the marking tasks.
    oop* old = p;
    oop obj = _heap->maybe_update_oop_ref(p);

#ifdef ASSERT
    if (ShenandoahTraceUpdates) {
      if (p != old) 
        tty->print_cr("Update "PTR_FORMAT" => "PTR_FORMAT"  to "PTR_FORMAT" => "PTR_FORMAT, p2i(p), p2i((HeapWord*) *p), p2i(old), p2i((HeapWord*) *old));
    }
#endif

    // NOTE: We used to assert the following here. This does not always work because
    // a concurrent Java thread could change the the field after we updated it.
    // oop obj = oopDesc::load_heap_oop(p);
    // assert(oopDesc::bs()->resolve_oop(obj) == *p, "we just updated the referrer");
    // assert(obj == NULL || ! _heap->heap_region_containing(obj)->is_dirty(), "must not point to dirty region");


    //  ShenandoahExtendedMarkObjsClosure cl(_heap->ref_processor_cm(), _worker_id);
    //  ShenandoahMarkObjsClosure mocl(cl, _worker_id);

    if (obj != NULL) {
      _mark_objs.do_object(obj);
    }
  }
};

class ShenandoahMarkRefsNoUpdateClosure : public ExtendedOopClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
  ShenandoahMarkObjsClosure _mark_objs;

public:
  ShenandoahMarkRefsNoUpdateClosure(uint worker_id) :
    ExtendedOopClosure(((ShenandoahHeap *) Universe::heap())->ref_processor_cm()),
    _worker_id(worker_id),
    _heap(ShenandoahHeap::heap()),
    _mark_objs(ShenandoahMarkObjsClosure(worker_id))
  {
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

  void do_oop(oop* p) {
    do_oop_work(p);
  }

private:
  void do_oop_work(oop* p) {
    oop obj = *p;
    if (! oopDesc::is_null(obj)) {
#ifdef ASSERT
      if (obj != oopDesc::bs()->resolve_oop(obj)) {
        oop obj_prime = oopDesc::bs()->resolve_oop(obj);
        tty->print_cr("We've got one: obj = "PTR_FORMAT" : obj_prime = "PTR_FORMAT, p2i((HeapWord*) obj), p2i((HeapWord*) obj_prime));
      }
      ResourceMark rm;
#endif
      assert(obj == oopDesc::bs()->resolve_oop(obj), err_msg("only mark forwarded copy of objects, obj: "PTR_FORMAT", obj_prime: "PTR_FORMAT", obj-klass: %s", p2i((HeapWord*) obj), p2i((HeapWord*) oopDesc::bs()->resolve_oop(obj)), obj->klass()->internal_name()));
      if (ShenandoahTraceConcurrentMarking) {
        tty->print_cr("Calling ShenandoahMarkRefsNoUpdateClosure on "PTR_FORMAT, p2i((HeapWord*) obj));
        ShenandoahHeap::heap()->print_heap_locations((HeapWord*) obj, (HeapWord*) obj + obj->size());
      }

      _mark_objs.do_object(obj);
    }
  }
};

class ShenandoahMarkRootsTask : public AbstractGangTask {
private:
  bool _update_refs;
public:
  ShenandoahMarkRootsTask(bool update_refs) :
    AbstractGangTask("Shenandoah update roots task"), _update_refs(update_refs) {
  }

  void work(uint worker_id) {
    // tty->print_cr("start mark roots worker: "INT32_FORMAT, worker_id);
    ExtendedOopClosure* cl;
    ShenandoahMarkRefsClosure rootsCl1(worker_id);
    ShenandoahMarkRefsNoUpdateClosure rootsCl2(worker_id);
    if (_update_refs) {
      cl = &rootsCl1;
    } else {
      cl = &rootsCl2;
    }

    CodeBlobToOopClosure blobsCl(cl, true);
    CLDToOopClosure cldCl(cl);

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ResourceMark m;
    heap->process_all_roots(false, SharedHeap::SO_AllCodeCache, cl, &cldCl, &blobsCl);

    // tty->print_cr("finish mark roots worker: "INT32_FORMAT, worker_id);
  }
};

class SCMConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
  int _seed;
  bool _update_refs;

public:
  SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator, bool update_refs) :
    AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator), _update_refs(update_refs), _seed(17) {
  }

      
  void work(uint worker_id) {

    ShenandoahMarkRefsClosure cl1(worker_id);
    ShenandoahMarkRefsNoUpdateClosure cl2(worker_id);
    ExtendedOopClosure* cl;

    if (_update_refs) {
      cl = &cl1;
    } else {
      cl = &cl2;
    }

    while (true) {
      if (!_cm->try_queue(worker_id,cl) &&
	  !_cm->try_overflow_queue(worker_id, cl) &&
	  !_cm->try_to_steal(worker_id, cl, &_seed) &&
	  !_cm->try_draining_an_satb_buffer(worker_id)) {
	if (_terminator->offer_termination()) break;
      }
    }
  }
};

void ShenandoahConcurrentMark::prepare_unmarked_root_objs() {

  if (! ShenandoahUpdateRefsEarly) {
    COMPILER2_PRESENT(DerivedPointerTable::clear());
  }

  prepare_unmarked_root_objs_no_derived_ptrs(! ShenandoahUpdateRefsEarly);

  if (! ShenandoahUpdateRefsEarly) {
    COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
  }

}

void ShenandoahConcurrentMark::prepare_unmarked_root_objs_no_derived_ptrs(bool update_refs) {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (ShenandoahParallelRootScan) {
    ClassLoaderDataGraph::clear_claimed_marks();
    heap->conc_workers()->set_active_workers(_max_conc_worker_id);
    heap->set_par_threads(heap->conc_workers()->active_workers()); // Prepare for parallel processing.
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
    SharedHeap::StrongRootsScope strong_roots_scope(heap, true);
    ShenandoahMarkRootsTask mark_roots(update_refs);
    heap->conc_workers()->run_task(&mark_roots);
    heap->set_par_threads(0); // Prepare for serial processing in future calls to process_strong_roots.
    if (ShenandoahProcessReferences) {
      ReferenceProcessor* rp = heap->ref_processor_cm();
      ExtendedOopClosure* cl;
      ShenandoahMarkRefsClosure rootsCl1(0);
      ShenandoahMarkRefsNoUpdateClosure rootsCl2(0);
      if (update_refs) {
        cl = &rootsCl1;
      } else {
        cl = &rootsCl2;
      }
      rp->weak_oops_do(cl);
    }
  } else {
    ExtendedOopClosure* cl;
    ShenandoahMarkRefsClosure rootsCl1(0);
    ShenandoahMarkRefsNoUpdateClosure rootsCl2(0);
    if (update_refs) {
      cl = &rootsCl1;
    } else {
      cl = &rootsCl2;
    }
    heap->roots_iterate(cl);
  }
  // tty->print_cr("all root marker threads done");
}


bool ShenandoahConcurrentMark::try_queue(uint worker_id, ExtendedOopClosure* cl) {
  oop obj;
  if (task_queues()->queue(worker_id % _max_conc_worker_id)->pop_local(obj)) {
    traverse_object(cl, obj);
    return true;
  } else 
    return false;;
}

bool ShenandoahConcurrentMark::try_to_steal(uint worker_id, ExtendedOopClosure* cl, int *seed) {
  oop obj;
  if (task_queues()->steal(worker_id, seed, obj)) {
    traverse_object(cl, obj);
    return true;
  } else 
    return false;
}

bool ShenandoahConcurrentMark::try_overflow_queue(uint worker_id, ExtendedOopClosure* cl) {
  oop obj = overflow_queue()->pop();
  if (obj != NULL) {
    traverse_object(cl, obj);
    return true;
  } else
    return false;
}

bool ShenandoahConcurrentMark:: try_draining_an_satb_buffer(uint worker_id) {
  return drain_one_satb_buffer(worker_id);
}


void ShenandoahConcurrentMark::traverse_object(ExtendedOopClosure* cl, oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();

  assert(sh->is_in(obj), "Should only traverse objects in the heap");

  if (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    assert(! sh->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");
    obj->oop_iterate(cl);
  }
}

void ShenandoahConcurrentMark::initialize() {
  _max_conc_worker_id = MAX2((uint) ConcGCThreads, 1U);
  _task_queues = new SCMObjToScanQueueSet((int) _max_conc_worker_id);
  _overflow_queue = new SharedOverflowMarkQueue();

  for (uint i = 0; i < _max_conc_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
  JavaThread::satb_mark_queue_set().set_buffer_size(1014 /* G1SATBBufferSize */);
}

void ShenandoahConcurrentMark::mark_from_roots(bool update_refs) {
  if (ShenandoahGCVerbose) {
    tty->print_cr("STOPPING THE WORLD: before marking");
    tty->print_cr("Starting markFromRoots");
  }

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::conc_mark);
  ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);

  if (ShenandoahProcessReferences) {
    ReferenceProcessor* rp = sh->ref_processor_cm();
    // enable ("weak") refs discovery
    rp->enable_discovery(true /*verify_no_refs*/);
    rp->setup_policy(false); // snapshot the soft ref policy to be used in this cycle
  }
  
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator, update_refs);

  sh->conc_workers()->set_active_workers(_max_conc_worker_id);
  sh->conc_workers()->run_task(&markingTask);

  if (ShenandoahGCVerbose) {
    tty->print("total workers = %u finished workers = %u\n", 
	       sh->conc_workers()->started_workers(), 
	       sh->conc_workers()->finished_workers());
    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  }

  sh->conc_workers()->set_active_workers(_max_conc_worker_id);

  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing markFromRoots");
    tty->print_cr("RESUMING THE WORLD: after marking");
  }

  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::conc_mark);
}

class FinishDrainSATBBuffersTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
public:
  FinishDrainSATBBuffersTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator) :
    AbstractGangTask("Finish draining SATB buffers"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    _cm->drain_satb_buffers(worker_id, true);
  }
};

void ShenandoahConcurrentMark::finish_mark_from_roots() {
  if (ShenandoahGCVerbose) {
    tty->print_cr("Starting finishMarkFromRoots");
  }

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  // Trace any (new) unmarked root references.
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::rescan_roots);
  prepare_unmarked_root_objs();
  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::rescan_roots);
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::drain_satb);
  {
    ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);
    ShenandoahHeap::StrongRootsScope srs(sh);
    // drain_satb_buffers(0, true);
    FinishDrainSATBBuffersTask drain_satb_buffers(this, &terminator);
    sh->conc_workers()->set_active_workers(_max_conc_worker_id);
    sh->conc_workers()->run_task(&drain_satb_buffers);
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::drain_satb);
  }
  
  // Also drain our overflow queue.
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::drain_overflow);

  ShenandoahMarkRefsClosure cl1(0);
  ShenandoahMarkRefsNoUpdateClosure cl2(0);
  ExtendedOopClosure* cl;
  if (! ShenandoahUpdateRefsEarly) {
    cl = &cl1;
  } else {
    cl = &cl2;
  }
  oop obj = _overflow_queue->pop();
  while (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    obj->oop_iterate(cl);
    obj = _overflow_queue->pop();
  }

  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::drain_overflow);

  // Finally mark everything else we've got in our queues during the previous steps.
  {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::drain_queues);
    ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);
    SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator, !ShenandoahUpdateRefsEarly);
    sh->conc_workers()->set_active_workers(_max_conc_worker_id);
    sh->conc_workers()->run_task(&markingTask);
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::drain_queues);
  }

#ifdef ASSERT
  for (int i = 0; i < (int) _max_conc_worker_id; i++) {
    assert(_task_queues->queue(i)->is_empty(), "Should be empty");
  }
#endif

  // When we're done marking everything, we process weak references.
  if (ShenandoahProcessReferences) {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::weakrefs);
    weak_refs_work();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::weakrefs);
  }

#ifdef ASSERT
  for (int i = 0; i < (int) _max_conc_worker_id; i++) {
    assert(_task_queues->queue(i)->is_empty(), "Should be empty");
  }
#endif

  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing finishMarkFromRoots");
#ifdef SLOWDEBUG
    for (int i = 0; i <(int)_max_conc_worker_id; i++) {
      tty->print("Queue: "INT32_FORMAT":", i);
      _task_queues->queue(i)->stats.print(tty, 10);
      tty->cr();
      _task_queues->queue(i)->stats.verify();
    }
#endif
  }
#ifdef ASSERT
  if (ShenandoahDumpHeapAfterConcurrentMark) {
    sh->ensure_parsability(false);
    sh->print_all_refs("post-mark");
  }
#endif
}

class ShenandoahSATBMarkObjsClosure : public ObjectClosure {
  uint _worker_id;
  ShenandoahMarkObjsClosure _wrapped;

public:
  ShenandoahSATBMarkObjsClosure(uint worker_id) :
    _worker_id(worker_id),
    _wrapped(ShenandoahMarkObjsClosure(worker_id)) { }

  void do_object(oop obj) {
    obj = ShenandoahBarrierSet::resolve_oop_static(obj);
    _wrapped.do_object(obj);
  }

};

void ShenandoahConcurrentMark::drain_satb_buffers(uint worker_id, bool remark) {

  // tty->print_cr("start draining SATB buffers");

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahSATBMarkObjsClosure cl(worker_id);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  satb_mq_set.set_closure(worker_id, &cl);
  while (satb_mq_set.apply_closure_to_completed_buffer(worker_id));

  if (remark) {
    satb_mq_set.par_iterate_closure_all_threads(worker_id);
    assert(satb_mq_set.completed_buffers_num() == 0, "invariant");
  }

  satb_mq_set.set_closure(worker_id, NULL);

  // tty->print_cr("end draining SATB buffers");

}

bool ShenandoahConcurrentMark::drain_one_satb_buffer(uint worker_id) {

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahSATBMarkObjsClosure cl(worker_id);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  satb_mq_set.set_closure(worker_id, &cl);
  bool result = satb_mq_set.apply_closure_to_completed_buffer(worker_id);
  satb_mq_set.set_closure(worker_id, NULL);
  return result;
}

void ShenandoahConcurrentMark::add_task(oop obj, int q) {
  assert(obj->is_oop(), "Oops, not an oop");
  assert(! ShenandoahHeap::heap()->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");

  assert(ShenandoahHeap::heap()->is_in((HeapWord*) obj), "Only push heap objects on the queue");
#ifdef ASSERT
  if (ShenandoahTraceConcurrentMarking){
    tty->print_cr("Adding object "PTR_FORMAT" to marking queue "INT32_FORMAT, p2i((HeapWord*) obj), q);
  }
#endif

  if (!_task_queues->queue(q)->push(obj)) {
    //    tty->print_cr("WARNING: Shenandoah mark queues overflown overflow_queue: obj = "PTR_FORMAT, p2i((HeapWord*) obj));
    _overflow_queue->push(obj);
  }
}

SharedOverflowMarkQueue* ShenandoahConcurrentMark::overflow_queue() {
  return _overflow_queue;
}

#if TASKQUEUE_STATS
void ShenandoahConcurrentMark::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ShenandoahConcurrentMark::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i); 
    _task_queues->queue(i)->stats.print(st);
    st->print("\n");
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();
  DEBUG_ONLY(totals.verify());

}

void ShenandoahConcurrentMark::print_push_only_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i); 
    _task_queues->queue(i)->stats.print(st);
    st->print("\n");
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify_only_pushes());
}

void ShenandoahConcurrentMark::reset_taskqueue_stats() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    _task_queues->queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

// Weak Reference Closures
class ShenandoahCMDrainMarkingStackClosure: public VoidClosure {
  ShenandoahHeap* _sh;
  ShenandoahConcurrentMark* _scm;
  uint _worker_id;

public:
  ShenandoahCMDrainMarkingStackClosure(uint worker_id): _worker_id(worker_id) {
    _sh = (ShenandoahHeap*) Universe::heap();
    _scm = _sh->concurrentMark();
  }

      
  void do_void() {

    ShenandoahMarkRefsClosure cl1(_worker_id);
    ShenandoahMarkRefsNoUpdateClosure cl2(_worker_id);
    ExtendedOopClosure* cl;

    if (! ShenandoahUpdateRefsEarly) {
      cl = &cl1;
    } else {
      cl = &cl2;
    }

    while (true) {
      if (!_scm->try_queue(_worker_id, cl) &&
	  !_scm->try_overflow_queue(_worker_id, cl) &&
	  !_scm->try_draining_an_satb_buffer(_worker_id)) {
	break;
      }
    }
  }
};


class ShenandoahCMKeepAliveAndDrainClosure: public OopClosure {
  uint _worker_id;
  ShenandoahHeap* _sh;
  ShenandoahConcurrentMark* _scm;

  size_t _ref_count;

public:
  ShenandoahCMKeepAliveAndDrainClosure(uint worker_id) {
    _worker_id = worker_id;
    _sh = (ShenandoahHeap*) Universe::heap();
    _scm = _sh->concurrentMark();
    _ref_count = 0;
  }

  virtual void do_oop(oop* p){ do_oop_work(p);}
  virtual void do_oop(narrowOop* p) {  
    assert(false, "narrowOops Aren't implemented");
  }


  void do_oop_work(oop* p) {  

    oop obj;
    if (! ShenandoahUpdateRefsEarly) {
      obj = _sh->maybe_update_oop_ref(p);
    } else {
      obj = oopDesc::load_heap_oop(p);
    }

    assert(obj == oopDesc::bs()->resolve_oop(obj), "only get updated oops in weak ref processing");

    if (obj != NULL) {
      if (Verbose && ShenandoahTraceWeakReferences) {
	gclog_or_tty->print_cr("\t["UINT32_FORMAT"] we're looking at location "
			       "*"PTR_FORMAT" = "PTR_FORMAT,
			       _worker_id, p2i(p), p2i((void*) obj));
	obj->print();
      }

      _sh->mark_current(obj);
      _scm->add_task(obj, _worker_id);

      _ref_count++;
    }    
  }

  size_t ref_count() { return _ref_count; }

};

class ShenandoahRefProcTaskProxy : public AbstractGangTask {

private:
  AbstractRefProcTaskExecutor::ProcessTask& _proc_task;

public:

  ShenandoahRefProcTaskProxy(AbstractRefProcTaskExecutor::ProcessTask& proc_task) :
    AbstractGangTask("Process reference objects in parallel"),
    _proc_task(proc_task) {
  }

  void work(uint worker_id) {
    ShenandoahIsAliveClosure is_alive;
    ShenandoahCMKeepAliveAndDrainClosure keep_alive(worker_id);
    ShenandoahCMDrainMarkingStackClosure complete_gc(worker_id);
    _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
  }
};

class ShenandoahRefEnqueueTaskProxy : public AbstractGangTask {

private:
  AbstractRefProcTaskExecutor::EnqueueTask& _enqueue_task;

public:

  ShenandoahRefEnqueueTaskProxy(AbstractRefProcTaskExecutor::EnqueueTask& enqueue_task) :
    AbstractGangTask("Enqueue reference objects in parallel"),
    _enqueue_task(enqueue_task) {
  }

  void work(uint worker_id) {
    _enqueue_task.work(worker_id);
  }
};

class ShenandoahRefProcTaskExecutor : public AbstractRefProcTaskExecutor {

private:
  WorkGang* _workers;

public:

  ShenandoahRefProcTaskExecutor() : _workers(ShenandoahHeap::heap()->conc_workers()) {
  }

  // Executes a task using worker threads.
  void execute(ProcessTask& task) {
    ShenandoahRefProcTaskProxy proc_task_proxy(task);
    _workers->run_task(&proc_task_proxy);
  }

  void execute(EnqueueTask& task) {
    ShenandoahRefEnqueueTaskProxy enqueue_task_proxy(task);
    _workers->run_task(&enqueue_task_proxy);
  }
};


void ShenandoahConcurrentMark::weak_refs_work() {
   ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
   ReferenceProcessor* rp = sh->ref_processor_cm();

   // Setup collector policy for softref cleaning.
   bool clear_soft_refs = sh->collector_policy()->use_should_clear_all_soft_refs(true /* bogus arg*/);
   if (ShenandoahTraceWeakReferences) {
     tty->print_cr("clearing soft refs: %s", BOOL_TO_STR(clear_soft_refs));
   }
   rp->setup_policy(clear_soft_refs);

   uint serial_worker_id = 0;
   ShenandoahIsAliveClosure is_alive;
   ShenandoahCMKeepAliveAndDrainClosure keep_alive(serial_worker_id);
   ShenandoahCMDrainMarkingStackClosure complete_gc(serial_worker_id);
   ShenandoahRefProcTaskExecutor par_task_executor;
   bool processing_is_mt = true;
   AbstractRefProcTaskExecutor* executor = (processing_is_mt ? &par_task_executor : NULL);


   // no timing for now.
   ConcurrentGCTimer gc_timer;

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("start processing references");
   }

   rp->process_discovered_references(&is_alive, &keep_alive, 
				     &complete_gc, &par_task_executor, &gc_timer,
                                     ShenandoahHeap::heap()->tracer()->gc_id());
   
   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished processing references, processed "SIZE_FORMAT" refs", keep_alive.ref_count());
     gclog_or_tty->print_cr("start enqueuing references");
   }

   rp->enqueue_discovered_references(executor);

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished enqueueing references");
   }

   rp->verify_no_references_recorded();
   assert(!rp->discovery_enabled(), "Post condition");

  // Now clean up stale oops in StringTable
   StringTable::unlink(&is_alive);
  // Clean up unreferenced symbols in symbol table.
   SymbolTable::unlink();
}


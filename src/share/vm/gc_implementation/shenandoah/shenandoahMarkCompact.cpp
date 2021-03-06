/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/sharedHeap.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"

ShenandoahMarkCompact::ShenandoahMarkCompact() :
  _heap(ShenandoahHeap::heap()),
  _barrier_set(ShenandoahMarkCompactBarrierSet()),
  _max_worker_id(MAX2((uint) ParallelGCThreads, 1U)),
  _seed(17)
{
  // nothing to do here.
}

void ShenandoahMarkCompact::initialize() {
  _max_worker_id = MAX2((uint) ParallelGCThreads, 1U);
  _task_queues = new ObjToScanQueueSet((int) _max_worker_id);
  _overflow_queue = new SharedOverflowMarkQueue();

  for (uint i = 0; i < _max_worker_id; ++i) {
    ObjToScanQueue* task_queue = new ObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
}
  
void ShenandoahMarkCompact::do_mark_compact() {

  BarrierSet* old_bs = oopDesc::bs();
  oopDesc::set_bs(&_barrier_set);

  assert(Thread::current()->is_VM_thread(), "Do full GC only while world is stopped");

  assert(_heap->is_bitmap_clear(), "require cleared bitmap");
  assert(!_heap->concurrent_mark_in_progress(), "can't do full-GC while marking is in progress");
  assert(!_heap->is_evacuation_in_progress(), "can't do full-GC while evacuation is in progress");
  assert(!_heap->is_update_references_in_progress(), "can't do full-GC while updating of references is in progress");

  _heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::full_gc);

  // We need to clear the is_in_collection_set flag in all regions.
  ShenandoahHeapRegion** regions = _heap->heap_regions();
  size_t num_regions = _heap->num_regions();
  for (size_t i = 0; i < num_regions; i++) {
    regions[i]->set_is_in_collection_set(false);
  }
  _heap->clear_cset_fast_test();

  if (ShenandoahVerify) {
    // Full GC should only be called between regular concurrent cycles, therefore
    // those verifications should be valid.
    _heap->verify_heap_after_evacuation();
    _heap->verify_heap_after_update_refs();
  }

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 1: marking the heap");
  }

  if (UseTLAB) {
    _heap->ensure_parsability(true);
  }

  phase1_mark_heap();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 2: calculating target addresses");
  }
  phase2_calculate_target_addresses();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 3: updating references");
  }
  phase3_update_references();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 4: compacting objects");
  }
  phase4_compact_objects();

  oopDesc::set_bs(old_bs);

  if (ShenandoahVerify) {
    _heap->verify_heap_after_evacuation();
    _heap->verify_heap_after_update_refs();
  }

  _heap->reset_mark_bitmap();

  _heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::full_gc);
}


void ShenandoahMarkCompact::add_task(oop obj, int q) {
  if (!_task_queues->queue(q)->push(obj)) {
    tty->print_cr("WARNING: Shenandoah mark queues overflown overflow_queue: obj = "PTR_FORMAT, p2i((HeapWord*) obj));
    _overflow_queue->push(obj);
  }
}

oop ShenandoahMarkCompact::pop_task(int q) {
  oop obj;
  if (_task_queues->queue(q)->pop_local(obj)) {
    return obj;
  }
  return NULL;
}

oop ShenandoahMarkCompact::steal_task(int q) {
  oop obj;
  if (_task_queues->steal(q, &_seed, obj)) {
    return obj;
  }
  return NULL;
}

class ShenandoahMarkCompactObjsClosure : public ExtendedOopClosure {
  uint _worker_id;
  ShenandoahMarkCompact* _smc;
  ShenandoahHeap* _heap;
public:
  ShenandoahMarkCompactObjsClosure(uint worker_id, ShenandoahMarkCompact* smc)  : 
    _worker_id(worker_id), 
    _smc(smc) 
  {
    _heap = ShenandoahHeap::heap();
  }

  void do_oop(oop* p) {
    oop obj = *p;
    if (obj != NULL) {
      if (_heap->mark_current_no_checks(obj)) {
	//	tty->print("thread %d is marking object %p\n", _worker_id, obj);
	_smc->add_task(obj, _worker_id);
      }
    }
  }

  void do_oop(narrowOop* oop) {
    assert(false, "Narrow Ooops aren't supported in Shenandoah GC");
  }
};

oop ShenandoahMarkCompact::get_task(int worker_id) {
  oop result = pop_task(worker_id);
  if (result == NULL) result = steal_task(worker_id);
  if (result == NULL) result = _overflow_queue->pop();
  return result;
}
  
class ShenandoahMarkTask : public AbstractGangTask {
  ShenandoahMarkCompact* _smc;
  ParallelTaskTerminator* _terminator;

public:
  ShenandoahMarkTask(ShenandoahMarkCompact *smc, 
		     ParallelTaskTerminator* terminator) : 
    AbstractGangTask("Shenandoah marking task"),
    _smc(smc),
    _terminator(terminator){
    // nothing to do here
  }

  void work(uint worker_id) {

    ResourceMark rm;

    ShenandoahMarkCompactObjsClosure cl(worker_id, _smc);
    CodeBlobToOopClosure blobsCl(&cl, true);
    CLDToOopClosure cldCl(&cl);


    ShenandoahHeap* heap = ShenandoahHeap::heap();
    // Start at the roots
    heap->process_all_roots(false, SharedHeap::SO_AllCodeCache, &cl, &cldCl, &blobsCl);

    while (true) {
    oop obj = _smc->get_task(worker_id);
    while (obj != NULL) {
      obj->oop_iterate(&cl);
      obj = _smc->get_task(worker_id);
    }
    if (_terminator->offer_termination()) break;
    }
  }
};


void ShenandoahMarkCompact::phase1_mark_heap() {
  ClassLoaderDataGraph::clear_claimed_marks();
  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);

  SharedHeap::StrongRootsScope strong_roots_scope(_heap, true);
  _heap->workers()->set_active_workers(_max_worker_id);
  _heap->set_par_threads(_max_worker_id);
  ShenandoahMarkTask mark_task(this, &terminator);
  _heap->workers()->run_task(&mark_task);
  if (ShenandoahGCVerbose) {
    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  }
  _heap->set_par_threads(0); // Prepare for serial processing in future calls to process_strong_roots.
}


class CalculateTargetAddressObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* _heap;

  HeapWord* _next_free_address;

  ShenandoahHeapRegion** _current_region;
  ShenandoahHeapRegion** _next_region;

#ifdef ASSERT
  ShenandoahHeapRegion** _regions;
  size_t _num_regions;
#endif

public:
  CalculateTargetAddressObjectClosure() :
    _heap(ShenandoahHeap::heap()) { 

    ShenandoahHeapRegion** regions = _heap->heap_regions();
    _current_region = regions;
    _next_region = _current_region + 1;
    _next_free_address = (*_current_region)->bottom();
#ifdef ASSERT
    _num_regions = _heap->num_regions();
    _regions = regions;
#endif
  }

  void do_object(oop p) {
    if (_heap->is_marked_current(p)) {
      if (_heap->heap_region_containing(p)->is_humonguous()) {
        // tty->print_cr("humonguous object in full GC");
        do_humonguous_object(p);
      } else {
        do_normal_object(p);
      }
    }
  }

private:

  void do_humonguous_object(oop p) {
    assert(_heap->isMarkedCurrent(p), "expect marked object");
    assert(_heap->heap_region_containing(p)->is_humonguous_start(), "expect humonguous start region");

    size_t obj_size = p->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    size_t required_regions = (obj_size * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes + 1;

    HeapWord* addr = (*_next_region)->bottom();
    BrooksPointer::get(p).set_forwardee(oop(addr + 1));
    _next_region += required_regions;
    assert(_next_region - 1 < _regions + _num_regions, "no overflow on regions");
  }

  void do_normal_object(oop p) {
    assert(_heap->isMarkedCurrent(p), "expect marked object");
    assert(! _heap->heap_region_containing(p)->is_humonguous(), "expect non-humonguous object");

    // Required space is object size plus brooks pointer.
    size_t obj_size = p->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    assert((*_current_region)->end() >= _next_free_address, "expect next address to be within region");
    assert(_next_free_address <= ((HeapWord*) p) - BrooksPointer::BROOKS_POINTER_OBJ_SIZE, "target address > obj");
    if (_next_free_address + obj_size > (*_current_region)->end()) {
      // tty->print_cr("skipping to next region. obj_size="INT32_FORMAT", current-region-end: "PTR_FORMAT", _next_free_addr: "PTR_FORMAT", free: "SIZ_EiFORMAT, obj_size, (*_current_region)->end(), _next_free_address, ((*_current_region)->end() - _next_free_address));
      // Skip to next region.
      _current_region = _next_region;
      assert(_current_region < _regions + _num_regions, "no overflow on regions");

      // tty->print_cr("skip to next region: "PTR_FORMAT, _current_region);
      _next_region++;
      // tty->print_cr("skip next region to: "PTR_FORMAT, _next_region);
      _next_free_address = (*_current_region)->bottom();
    }
    // tty->print_cr("forward object at: "PTR_FORMAT" to new location: "PTR_FORMAT", current-region-end: "PTR_FORMAT, (HeapWord*) p, (HeapWord*)(_next_free_address + 1), (*_current_region)->end());
    // We keep the pointer to the new location in the object's brooks ptr field, not the pointer
    // to the (new) brooks pointer location.
    assert(_next_free_address + 1 <= ((HeapWord*) p), "new object address must be <= original address");
    BrooksPointer::get(p).set_forwardee(oop(_next_free_address + 1));
    _next_free_address += obj_size;
    assert(_next_free_address <= (*_current_region)->end(), "next free address must be within current region");
  }

};

void ShenandoahMarkCompact::phase2_calculate_target_addresses() {

  CalculateTargetAddressObjectClosure cl;
  _heap->object_iterate(&cl);
}

class ShenandoahMarkCompactUpdateRefsClosure : public ExtendedOopClosure {
  void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      assert(ShenandoahHeap::heap()->is_in(obj), "old object location must be inside heap");
      assert(ShenandoahHeap::heap()->is_marked_current(obj), "only update references to marked objects");
      assert(obj->is_oop(), "expect oop");
      oop new_obj = BrooksPointer::get(obj).get_forwardee_raw();
      assert(ShenandoahHeap::heap()->is_in(new_obj), "new object location must be inside heap");
      assert((HeapWord*) new_obj <= (HeapWord*) obj, "new object location must be down in the heap");
      // tty->print_cr("update reference at: "PTR_FORMAT" pointing to: "PTR_FORMAT" to new location at: "PTR_FORMAT, p, obj, new_obj);
      oopDesc::store_heap_oop(p, new_obj);
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

void ShenandoahMarkCompact::phase3_update_references() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  ShenandoahMarkCompactUpdateRefsClosure cl;

  // tty->print_cr("updating strong roots");
  CodeBlobToOopClosure blobsCl(&cl, true);
  CLDToOopClosure cldCl(&cl);
  ClassLoaderDataGraph::clear_claimed_marks();
  _heap->process_all_roots(true, SharedHeap::SO_AllCodeCache, &cl, &cldCl, &blobsCl);
  _heap->process_weak_roots(&cl);
  _heap->oop_iterate(&cl, false, true);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

}

class ShenandoahCompactObjectsClosure : public ObjectClosureCareful {
private:
  ShenandoahHeap* _heap;
  HeapWord* _last_addr;
  size_t _used;
public:
  ShenandoahCompactObjectsClosure() :
    _heap(ShenandoahHeap::heap()),
    _last_addr(_heap->heap_regions()[0]->bottom()),
    _used(0) {
  }

  void do_object(oop p) {
    Unimplemented();
  }
  size_t do_object_careful_m(oop p, MemRegion mr) {
    Unimplemented();
    return 0;
  }

  size_t do_object_careful(oop p) {
    size_t obj_size = p->size();
    if (_heap->is_marked_current(p)) {
      _used += obj_size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
      HeapWord* old_addr = (HeapWord*) p;
      HeapWord* new_addr = (HeapWord*) BrooksPointer::get(p).get_forwardee_raw();
      assert(new_addr <= old_addr, "new location must not be higher up in the heap");
      // tty->print_cr("copying object size "INT32_FORMAT" from "PTR_FORMAT" to "PTR_FORMAT, obj_size, old_addr, new_addr);
      oop new_obj = oop(new_addr);
      if (new_addr != old_addr) {
        if (new_addr + obj_size < old_addr) {
          Copy::disjoint_words(old_addr, new_addr, obj_size);
        } else {
          Copy::conjoint_words(old_addr, new_addr, obj_size);
        }
        BrooksPointer::get(new_obj).set_forwardee(new_obj);
      }

      if (obj_size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE > ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) {
        // tty->print_cr("finishing humonguous region");
        finish_humonguous_regions(oop(old_addr), new_obj);
      } else {
        assert(! _heap->heap_region_containing(p)->is_humonguous(), "expect non-humonguous object");
        _heap->heap_region_containing(new_addr)->set_top(new_addr + obj_size);
      }
      _last_addr = MAX2(new_addr + obj_size, _last_addr);
    }
    return obj_size;
  }

  void finish_humonguous_regions(oop old_obj, oop new_obj) {
    // tty->print_cr("finish humonguous object: "PTR_FORMAT" -> "PTR_FORMAT, old_obj, new_obj);
    size_t obj_size = new_obj->size();
    size_t required_regions = (obj_size * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes + 1;

    size_t remaining_size = obj_size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    for (uint i = 0; i < required_regions; i++) {
      HeapWord* ptr = ((HeapWord*) new_obj) + i * (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize);
      ShenandoahHeapRegion* region = _heap->heap_region_containing(ptr);
      size_t region_size = MIN2(remaining_size, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize);
      assert(region_size != 0, "no empty humonguous regions");
      region->set_top(region->bottom() + region_size);
      remaining_size -= region_size;
    }
    //tty->print_cr("end addr of humonguous object: %p", (((HeapWord*) new_obj) + obj_size), _last_addr);
  }

  HeapWord* last_addr() {
    return _last_addr;
  }

  size_t used() {
    return _used;
  }
};

void ShenandoahMarkCompact::finish_compaction(HeapWord* last_addr) {

  // Recycle all unused regions.
  ShenandoahHeapRegion** regions = _heap->heap_regions();
  size_t num_regions = _heap->num_regions();
  ShenandoahHeapRegionSet* free_regions = _heap->free_regions();
  free_regions->clear();
  uint remaining_humonguous_continuations = 0;
  for (uint i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = regions[i];
    region->clearLiveData();
    if (remaining_humonguous_continuations > 0) {
      region->set_humonguous_continuation(true);
      region->set_humonguous_start(false);
      assert(region->used() > 0, "no empty humonguous region");
      remaining_humonguous_continuations--;
      continue;
    }
    if (region->bottom() > last_addr) {
      // tty->print_cr("recycling region after full-GC: %d", region->region_number());
      region->reset();
      free_regions->append(region);
      continue;
    }
    if (region->used() > 0) {
      oop first_obj = oop(region->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
      size_t obj_size = first_obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
      if (obj_size > ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) {
        // This is a humonguous object. Fix up the humonguous flags in this region and the following.
        region->set_humonguous_start(true);
        region->set_humonguous_continuation(false);
        assert(region->used() > 0, "no empty humonguous region");
        remaining_humonguous_continuations = (obj_size * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes;
      } else {
        region->set_humonguous_start(false);
        region->set_humonguous_continuation(false);
      }
    }
  }
}

class ResetSafeIterationLimitsClosure : public ShenandoahHeapRegionClosure {
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->set_concurrent_iteration_safe_limit(r->top());
    return false;
  }
};

void ShenandoahMarkCompact::phase4_compact_objects() {

  ResetSafeIterationLimitsClosure hrcl;
  _heap->heap_region_iterate(&hrcl);

  ShenandoahCompactObjectsClosure cl;
  _heap->object_iterate_careful(&cl);

  finish_compaction(cl.last_addr());

  _heap->set_used(cl.used() * HeapWordSize);
}

#if TASKQUEUE_STATS
void ShenandoahMarkCompact::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ShenandoahMarkCompact::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->max_workers();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i); 
    _task_queues->queue(i)->stats.print(st);
    st->print("\n");
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();
  DEBUG_ONLY(totals.verify());

}

void ShenandoahMarkCompact::reset_taskqueue_stats() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  //  const int n = sh->workers() != NULL ? sh->workers()->total_workers() : 1;
  const int n = sh->max_workers();
  for (int i = 0; i < n; ++i) {
    _task_queues->queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

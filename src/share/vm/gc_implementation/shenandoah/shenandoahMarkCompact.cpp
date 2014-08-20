/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/sharedHeap.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"

ShenandoahMarkCompact::ShenandoahMarkCompact() :
  _heap(ShenandoahHeap::heap()),
  _barrier_set(ShenandoahMarkCompactBarrierSet()) {

  // Nothing left to do.

}

void ShenandoahMarkCompact::do_mark_compact() {

  BarrierSet* old_bs = oopDesc::bs();
  oopDesc::set_bs(&_barrier_set);

  assert(Thread::current()->is_VM_thread(), "Do full GC only while world is stopped");

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 1: marking the heap");
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

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

  oopDesc::set_bs(old_bs);

  _heap->reset_mark_bitmap();
  if (ShenandoahVerify) {
    _heap->verify_heap_after_evacuation();
    _heap->verify_heap_after_update_refs();
  }
}

void ShenandoahMarkCompact::phase1_mark_heap() {

  if (UseTLAB) {
    _heap->ensure_parsability(true);
  }

  // TODO: Move prepare_unmarked_root_objs() into SCM!
  // TODO: Make this whole sequence a separate method in SCM!
  _heap->concurrentMark()->prepare_unmarked_root_objs_no_derived_ptrs();
  _heap->concurrentMark()->mark_from_roots();
  _heap->concurrentMark()->finish_mark_from_roots(true);

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
    assert((*_current_region)->end() > _next_free_address, "expect next address to be within region");
    if (obj_size > (size_t) ((*_current_region)->end() - _next_free_address)) {
      // tty->print_cr("skipping to next region. obj_size=%d, current-region-end: %p, _next_free_addr: %p, free: %d", obj_size, (*_current_region)->end(), _next_free_address, ((*_current_region)->end() - _next_free_address));
      // Skip to next region.
      _current_region = _next_region;
      assert(_current_region < _regions + _num_regions, "no overflow on regions");

      // tty->print_cr("skip to next region: %p", _current_region);
      _next_region++;
      // tty->print_cr("skip next region to: %p", _next_region);
      _next_free_address = (*_current_region)->bottom();
    }
    // tty->print_cr("forward object at: %p to new location: %p, current-region-end: %p", (HeapWord*) p, (HeapWord*)(_next_free_address + 1), (*_current_region)->end());
    // We keep the pointer to the new location in the object's brooks ptr field, not the pointer
    // to the (new) brooks pointer location.
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
      assert(obj->is_oop(), "expect oop");
      oop new_obj = BrooksPointer::get(obj).get_forwardee_raw();
      assert((HeapWord*) new_obj <= (HeapWord*) obj, "new object location must be down in the heap");
      // tty->print_cr("update reference at: %p pointing to: %p to new location at: %p", p, obj, new_obj);
      oopDesc::store_heap_oop(p, new_obj);
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

void ShenandoahMarkCompact::phase3_update_references() {
  ShenandoahMarkCompactUpdateRefsClosure cl;
  // tty->print_cr("updating roots");
  CodeBlobToOopClosure blobsCl(&cl, false);
  KlassToOopClosure klassCl(&cl);
  ClassLoaderDataGraph::clear_claimed_marks();
  _heap->process_strong_roots(true, false, SharedHeap::SO_AllClasses, &cl, NULL, &klassCl);
  // tty->print_cr("updating weak roots");
  _heap->weak_roots_iterate(&cl);
  // tty->print_cr("updating heap references");
  _heap->oop_iterate(&cl, false, true);
}

class ShenandoahCompactObjectsClosure : public ObjectClosureCareful {
private:
  ShenandoahHeap* _heap;
  HeapWord* _last_end_addr;
  HeapWord* _last_humonguous_end_addr;

public:
  ShenandoahCompactObjectsClosure() : _heap(ShenandoahHeap::heap()) {
    _last_end_addr = _heap->heap_regions()[0]->bottom();
    _last_humonguous_end_addr = _heap->heap_regions()[0]->bottom();
  }

  void do_object(oop p) {
    Unimplemented();
  }
  size_t do_object_careful_m(oop p, MemRegion mr) {
    Unimplemented();
  }

  size_t do_object_careful(oop p) {
    size_t obj_size = p->size();
    if (_heap->is_marked_current(p)) {
      HeapWord* old_addr = (HeapWord*) p;
      HeapWord* new_addr = (HeapWord*) BrooksPointer::get(p).get_forwardee_raw();
      assert(new_addr <= old_addr, "new location must not be higher up in the heap");
      // tty->print_cr("copying object from %p to %p", old_addr, new_addr);
      if (new_addr + obj_size < old_addr) {
        Copy::disjoint_words(old_addr, new_addr, obj_size);
      } else {
        Copy::conjoint_words(old_addr, new_addr, obj_size);
      }
      oop new_obj = oop(new_addr);
      BrooksPointer::get(new_obj).set_forwardee(new_obj);

      if (_heap->heap_region_containing(p)->is_humonguous_start()) {
        finish_humonguous_regions(new_obj);
      } else {
        assert(! _heap->heap_region_containing(p)->is_humonguous(), "expect non-humonguous object");
        maybe_finish_region((HeapWord*) new_obj);
      }
    }
    return obj_size;
  }

  void finish_humonguous_regions(oop new_obj) {
    size_t obj_size = new_obj->size();
    size_t required_regions = (obj_size * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes + 1;
    size_t remaining_size = obj_size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    for (uint i = 0; i < required_regions; i++) {
      HeapWord* ptr = ((HeapWord*) new_obj) + i * (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize);
      ShenandoahHeapRegion* region = _heap->heap_region_containing(ptr);
      size_t region_size = MIN2(remaining_size, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize);
      region->set_top(region->bottom() + region_size);
      remaining_size -= region_size;

      if (i == 0) {
        region->set_humonguous_start(true);
      } else {
        region->set_humonguous_continuation(true);
      }
      region->setLiveData(0);
    }

    _last_humonguous_end_addr = ((HeapWord*) new_obj) + new_obj->size();
  }

  void maybe_finish_region(HeapWord* new_addr) {
    ShenandoahHeapRegion* last_region = _heap->heap_region_containing(_last_end_addr);
    if (last_region != _heap->heap_region_containing(new_addr)) {
      last_region->set_top(_last_end_addr);
      last_region->setLiveData(0);
    }
    _last_end_addr = new_addr + oop(new_addr)->size();
  }

  HeapWord* last_addr() {
    return _last_end_addr;
  }

  HeapWord* last_humonguous_addr() {
    return _last_humonguous_end_addr;
  }

};

void ShenandoahMarkCompact::finish_compaction(HeapWord* last_addr, HeapWord* last_humonguous_addr) {

  // First finish last region that we compacted into.
  ShenandoahHeapRegion* last_region = _heap->heap_region_containing(last_addr);
  last_region->set_top(last_addr);
  last_region->setLiveData(0);

  // Recycle all unused regions.
  HeapWord* total_last_addr = MAX2(last_addr, last_humonguous_addr);
  ShenandoahHeapRegion** regions = _heap->heap_regions();
  size_t num_regions = _heap->num_regions();
  for (uint i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* region = regions[i];
    if (region->bottom() > total_last_addr) {
      region->reset();
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

  finish_compaction(cl.last_addr(), cl.last_humonguous_addr());
}

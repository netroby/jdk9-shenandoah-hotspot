/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/quickSort.hpp"

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions) :
  _max_regions(max_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {

  _current = &_regions[0];
  _next_free = &_regions[0];
  _concurrent_next_free = _next_free;
}

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions, ShenandoahHeapRegion** regions, size_t num_regions) :
  _max_regions(num_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {

  // Make copy of the regions array so that we can sort without destroying the original.
  memcpy(_regions, regions, sizeof(ShenandoahHeapRegion*) * num_regions);

  _current = &_regions[0];
  _next_free = &_regions[num_regions];
  _concurrent_next_free = _next_free;
}

ShenandoahHeapRegionSet::~ShenandoahHeapRegionSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions, mtGC);
}

int compareHeapRegionsByGarbage(ShenandoahHeapRegion* a, ShenandoahHeapRegion* b) {
  if (a == NULL) {
    if (b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (b == NULL) {
    return -1;
  }

  size_t garbage_a = a->garbage();
  size_t garbage_b = b->garbage();
  
  if (garbage_a > garbage_b) 
    return -1;
  else if (garbage_a < garbage_b)
    return 1;
  else return 0;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::current() {
  return *(limit_region(_current));
}

size_t ShenandoahHeapRegionSet::length() {
  return _next_free - _regions;
}

size_t ShenandoahHeapRegionSet::available_regions() {
  return (_regions + _max_regions) - _next_free;
}

void ShenandoahHeapRegionSet::append(ShenandoahHeapRegion* region) {
  assert(_next_free < _regions + _max_regions, "need space for additional regions");

  // Grab next slot.
  ShenandoahHeapRegion** next_free = ((ShenandoahHeapRegion**) Atomic::add_ptr(sizeof(ShenandoahHeapRegion**), &_concurrent_next_free)) - 1;

  // Insert new region into slot.
  *next_free = region;

  // Make slot visible to other threads.
  while (true) {
    ShenandoahHeapRegion** prev = (ShenandoahHeapRegion**) Atomic::cmpxchg_ptr(next_free + 1, &_next_free, next_free);
    if (prev == next_free) { // CAS succeeded.
      break;
    } // else CAS failed. A concurrent thread also inserts and we need to wait for it to update its slot.
  }
}

void ShenandoahHeapRegionSet::clear() {
  _current = _regions;
  _next_free = _regions;
  _concurrent_next_free = _regions;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  ShenandoahHeapRegion** next = (ShenandoahHeapRegion**) Atomic::add_ptr(sizeof(ShenandoahHeapRegion**), &_current);
  next--;
  if (next < _next_free) {
    return *next;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get_next() {
  ShenandoahHeapRegion** next = _current + 1;
  if (next < _next_free) {
    _current = next;
    return *next;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion** ShenandoahHeapRegionSet::limit_region(ShenandoahHeapRegion** region) {
  if (region >= _next_free) {
    return NULL;
  } else {
    return region;
  }
}

void ShenandoahHeapRegionSet::print() {
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    if (i == _current) {
      tty->print_cr("-->");
    }
    (*i)->print();
  }
}

void ShenandoahHeapRegionSet::choose_collection_and_free_sets(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set) {
  col_set->choose_collection_set(_regions, length());
  free_set->choose_free_set(_regions, length());
}

void ShenandoahHeapRegionSet::choose_collection_set(ShenandoahHeapRegion** regions, size_t length) {

  clear();

  assert(length <= _max_regions, "must not blow up array");

  ShenandoahHeapRegion** tmp = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, length, mtGC);

  memcpy(tmp, regions, sizeof(ShenandoahHeapRegion*) * length);

  QuickSort::sort<ShenandoahHeapRegion*>(tmp, length, compareHeapRegionsByGarbage, false);

  ShenandoahHeapRegion** r = tmp;
  ShenandoahHeapRegion** end = tmp + length;

  // We don't want the current allocation region in the collection set because a) it is still being allocated into and b) This is where the write barriers will allocate their copies.

  while (r < end) {
    ShenandoahHeapRegion* region = *r;
    if (region->garbage() > _garbage_threshold
        && ! (region->has_active_tlabs() || region->is_current_allocation_region()
              || region->is_humonguous())) {

      append(region);
      region->set_is_in_collection_set(true);
    }
    r++;
  }

  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, tmp, mtGC);

}

void ShenandoahHeapRegionSet::choose_free_set(ShenandoahHeapRegion** regions, size_t length) {

  clear();
  ShenandoahHeapRegion** end = regions + length;

  for (ShenandoahHeapRegion** r = regions; r < end; r++) {
    ShenandoahHeapRegion* region = *r;
    if ((! region->is_in_collection_set())
        && (! region->is_current_allocation_region())
        && (! region->is_humonguous())) {
      append(region);
    }
  }
}  

void ShenandoahHeapRegionSet::reclaim_humonguous_regions() {

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (ShenandoahHeapRegion** r = _regions; r < _next_free; r++) {
    // We can immediately reclaim humonguous objects/regions that are no longer reachable.
    ShenandoahHeapRegion* region = *r;
    if (region->is_humonguous_start()) {
      oop humonguous_obj = oop(region->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
      if (! heap->isMarkedCurrent(humonguous_obj)) {
        reclaim_humonguous_region_at(r);
      }
    }
  }

}

void ShenandoahHeapRegionSet::reclaim_humonguous_region_at(ShenandoahHeapRegion** r) {
  assert((*r)->is_humonguous_start(), "reclaim regions starting with the first one");
  if (ShenandoahGCVerbose) {
    tty->print_cr("recycling humonguous region:");
    (*r)->print();
  }

  oop humonguous_obj = oop((*r)->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
  size_t size = humonguous_obj->size();

  (*r)->recycle();
  for (ShenandoahHeapRegion** i = r + 1; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    if (region->is_humonguous_continuation()) {
      if (ShenandoahGCVerbose) {
        tty->print_cr("recycling humonguous region:");
        region->print();
      }
      region->recycle();
    } else {
      break;
    }
  }

  ShenandoahHeap::heap()->decrease_used(size);
}

void ShenandoahHeapRegionSet::set_concurrent_iteration_safe_limits() {
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    region->set_concurrent_iteration_safe_limit(region->top());
  }
}

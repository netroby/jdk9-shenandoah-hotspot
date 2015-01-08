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

  _next = &_regions[0];
  _current_allocation = NULL;
  _current_evacuation = NULL;
  _next_free = &_regions[0];
}

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions, ShenandoahHeapRegion** regions, size_t num_regions) :
  _max_regions(num_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {

  // Make copy of the regions array so that we can sort without destroying the original.
  memcpy(_regions, regions, sizeof(ShenandoahHeapRegion*) * num_regions);

  _next = &_regions[0];
  _current_allocation = NULL;
  _current_evacuation = NULL;
  _next_free = &_regions[num_regions];
}

ShenandoahHeapRegionSet::~ShenandoahHeapRegionSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions);
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

ShenandoahHeapRegion* ShenandoahHeapRegionSet::current(bool evacuation) {
  ShenandoahHeapRegion** current = evacuation ? _current_evacuation : _current_allocation;
  if (current == NULL) {
    return get_next(evacuation);
  } else {
    return *(limit_region(current));
  }
}

size_t ShenandoahHeapRegionSet::length() {
  return _next_free - _regions;
}

size_t ShenandoahHeapRegionSet::available_regions() {
  return (_regions + _max_regions) - _next_free;
}

void ShenandoahHeapRegionSet::append(ShenandoahHeapRegion* region) {
  assert(_next_free < _regions + _max_regions, "need space for additional regions");
  assert(SafepointSynchronize::is_at_safepoint() || ShenandoahHeap_lock->owned_by_self() || ! Universe::is_fully_initialized(), "only append regions to list while world is stopped");

  // Grab next slot.
  ShenandoahHeapRegion** next_free = _next_free;
  _next_free++;

  // Insert new region into slot.
  *next_free = region;
}

void ShenandoahHeapRegionSet::clear() {
  _current_allocation = NULL;
  _current_evacuation = NULL;
  _next = _regions;
  _next_free = _regions;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  ShenandoahHeapRegion** next = (ShenandoahHeapRegion**) Atomic::add_ptr(sizeof(ShenandoahHeapRegion**), &_next);
  next--;
  if (next < _next_free) {
    return *next;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get_next(bool evacuation) {

  ShenandoahHeapRegion** next = _next;
  if (next < _next_free) {
    if (evacuation) {
      _current_evacuation = next;
    } else {
      _current_allocation = next;
    }
    _next++;
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
    if (i == _current_allocation) {
      tty->print_cr("A->");
    }
    if (i == _current_evacuation) {
      tty->print_cr("E->");
    }
    if (i == _next) {
      tty->print_cr("N->");
    }
    (*i)->print();
  }
}

void ShenandoahHeapRegionSet::choose_collection_and_free_sets(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set) {
  col_set->choose_collection_set(_regions, length());
  free_set->choose_free_set(_regions, length());
}

void ShenandoahHeapRegionSet::choose_collection_and_free_sets_min_garbage(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set, size_t min_garbage) {
  col_set->choose_collection_set_min_garbage(_regions, length(), min_garbage);
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
    if (region->garbage() > _garbage_threshold && ! region->is_humonguous()) {

      assert(! region->is_humonguous(), "no humonguous regions in collection set");
      append(region);
      region->set_is_in_collection_set(true);
    }
    r++;
  }

  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, tmp);

}

void ShenandoahHeapRegionSet::choose_collection_set_min_garbage(ShenandoahHeapRegion** regions, size_t length, size_t min_garbage) {

  clear();

  assert(length <= _max_regions, "must not blow up array");

  ShenandoahHeapRegion** tmp = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, length, mtGC);

  memcpy(tmp, regions, sizeof(ShenandoahHeapRegion*) * length);

  QuickSort::sort<ShenandoahHeapRegion*>(tmp, length, compareHeapRegionsByGarbage, false);

  ShenandoahHeapRegion** r = tmp;
  ShenandoahHeapRegion** end = tmp + length;

  // We don't want the current allocation region in the collection set because a) it is still being allocated into and b) This is where the write barriers will allocate their copies.

  size_t garbage = 0;
  while (r < end && garbage < min_garbage) {
    ShenandoahHeapRegion* region = *r;
    if (region->garbage() > _garbage_threshold && ! region->is_humonguous()) {
      append(region);
      garbage += region->garbage();
      region->set_is_in_collection_set(true);
    }
    r++;
  }

  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, tmp);

  /*
  tty->print_cr("choosen region with "SIZE_FORMAT" garbage given "SIZE_FORMAT" min_garbage", garbage, min_garbage);
  */
}


void ShenandoahHeapRegionSet::choose_free_set(ShenandoahHeapRegion** regions, size_t length) {

  clear();
  ShenandoahHeapRegion** end = regions + length;

  for (ShenandoahHeapRegion** r = regions; r < end; r++) {
    ShenandoahHeapRegion* region = *r;
    if ((! region->is_in_collection_set())
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

  oop humonguous_obj = oop((*r)->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
  size_t size = humonguous_obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  uint required_regions = (size * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes  + 1;

  if (ShenandoahTraceHumonguous) {
    tty->print_cr("reclaiming "UINT32_FORMAT" humonguous regions for object of size: "SIZE_FORMAT" words", required_regions, size);
  }

  assert((*r)->getLiveData() == 0, "liveness must be zero");

  for (ShenandoahHeapRegion** i = r; i < r + required_regions; i++) {
    ShenandoahHeapRegion* region = *i;

    assert(i == r ? region->is_humonguous_start() : region->is_humonguous_continuation(),
           "expect correct humonguous start or continuation");

    if (ShenandoahTraceHumonguous) {
      region->print();
    }

    region->reset();
  }

  ShenandoahHeap::heap()->decrease_used(size * HeapWordSize);
}

void ShenandoahHeapRegionSet::set_concurrent_iteration_safe_limits() {
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    region->set_concurrent_iteration_safe_limit(region->top());
  }
}

size_t ShenandoahHeapRegionSet::garbage() {
  size_t garbage = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    garbage += region->garbage();
  }
  return garbage;
}

size_t ShenandoahHeapRegionSet::used() {
  size_t used = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    used += region->used();
  }
  return used;
}

size_t ShenandoahHeapRegionSet::live_data() {
  size_t live = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    live += region->getLiveData();
  }
  return live;
}

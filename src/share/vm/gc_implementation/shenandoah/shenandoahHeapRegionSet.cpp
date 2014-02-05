/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "memory/resourceArea.hpp"

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t num_regions) :
  _index(0),
  _inserted(0),
  _numRegions(num_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, num_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {
}

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t num_regions, ShenandoahHeapRegion** regions) :
  _index(0),
  _inserted(num_regions),
  _numRegions(num_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, num_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {

  // Make copy of the regions array so that we can sort without destroying the original.
  memcpy(_regions, regions, sizeof(ShenandoahHeapRegion*) * num_regions);

}

ShenandoahHeapRegionSet::~ShenandoahHeapRegionSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions, mtGC);
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::at(uint i) {
  return _regions[i];
}

size_t ShenandoahHeapRegionSet::length() {
  return _inserted;
}

size_t ShenandoahHeapRegionSet::available_regions() {
  return _inserted - _index;
}

int compareHeapRegionsByGarbage(ShenandoahHeapRegion** a, ShenandoahHeapRegion** b) {
  if (*a == NULL) {
    if (*b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (*b == NULL) {
    return -1;
  }

  size_t garbage_a = (*a)->garbage();
  size_t garbage_b = (*b)->garbage();
  
  if (garbage_a > garbage_b) 
    return -1;
  else if (garbage_a < garbage_b)
    return 1;
  else return 0;
}

int compareHeapRegionsByFree(ShenandoahHeapRegion** a, ShenandoahHeapRegion** b) {
  if (*a == NULL) {
    if (*b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (*b == NULL) {
    return -1;
  }

  size_t free_a = (*a)->free();
  size_t free_b = (*b)->free();
  
  if (free_a > free_b) 
    return -1;
  else if (free_a < free_b)
    return 1;
  else return 0;
}

void ShenandoahHeapRegionSet::put(size_t index, ShenandoahHeapRegion* region) {
  _regions[index] = region;

  // We need a memory barrier here, to make sure concurrent threads see the update
  // into the array _before_ the index gets updated. We concurrently insert
  // regions into the free-region list when creating new regions.
  OrderAccess::storestore();

  _inserted++;

  // This is not strictly necessary, but it seems good to make sure concurrent
  // threads also see the update to the index right away.
  OrderAccess::storeload();

}

void ShenandoahHeapRegionSet::append(ShenandoahHeapRegion* region) {
  put(_inserted, region);
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get_next() {
  ShenandoahHeapRegion* result = NULL;

  if (_index < _inserted) 
    result = _regions[_index++];

  return result;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::peek_next() {
  ShenandoahHeapRegion* result = NULL;

  int index = _index;
  while (index < _inserted) {
    result = _regions[index];
    // We need to claim regions otherwise we could return regions that are used
    // for humonguous objects.
    if (result->claim()) {
      if (! result->is_humonguous()) {
        _index = index;
        break;
      } else {
        result->clearClaim();
        index++;
      }
    } else {
      index++;
    }
  }
  return result;
}

bool ShenandoahHeapRegionSet::has_next() {
  return _index < _inserted - 1;
}

void ShenandoahHeapRegionSet::sortDescendingFree() {
  ResourceMark rm;

  GrowableArray<ShenandoahHeapRegion*>* rs = 
    new GrowableArray<ShenandoahHeapRegion*>();
  for (int i = 0; i < _inserted; i++)
    rs->append(_regions[i]);

  rs->sort(compareHeapRegionsByFree);

  for (int i = 0; i < _inserted; i++) 
    _regions[i] = rs->at(i);
}
   
void ShenandoahHeapRegionSet::sortDescendingGarbage() {
  ResourceMark rm;

  GrowableArray<ShenandoahHeapRegion*>* rs = 
    new GrowableArray<ShenandoahHeapRegion*>();

  for (int i = 0; i < _inserted; i++)
    rs->append(_regions[i]);

  rs->sort(compareHeapRegionsByGarbage);

  for (int i = 0; i < _inserted; i++) 
    _regions[i] = rs->at(i);
}
  
void ShenandoahHeapRegionSet::print() {
  for (int i = 0; i < _inserted; i++) {
    if (i == _index) {
      tty->print_cr("-->");
    }
    _regions[i]->print();
  }
}

void ShenandoahHeapRegionSet::
choose_collection_set(ShenandoahHeapRegionSet* region_set, 
		      int max_regions) {
  sortDescendingGarbage();
  int r = 0;
  int cs_index = 0;

  while (r < _numRegions && _regions[r]->garbage() > _garbage_threshold && cs_index <= max_regions) {
    if (! ( _regions[r]->has_active_tlabs() || _regions[r]->is_current_allocation_region()
            || _regions[r]->is_humonguous())) {
      region_set->_regions[cs_index] = _regions[r];
      _regions[r]->set_is_in_collection_set(true);
      cs_index++;
    }
    r++;
  }

  region_set->_inserted = cs_index;
  region_set->_index = 0;
  region_set->_numRegions = cs_index;
}

void ShenandoahHeapRegionSet::choose_collection_set(ShenandoahHeapRegionSet* region_set) {
  sortDescendingGarbage();
  int r = 0;
  int cs_index = 0;

  // We don't want the current allocation region in the collection set because a) it is still being allocated into and b) This is where the write barriers will allocate their copies.

  while (r < _numRegions && _regions[r]->garbage() > _garbage_threshold) {
    if (! ( _regions[r]->has_active_tlabs() || _regions[r]->is_current_allocation_region()
            || _regions[r]->is_humonguous())) {
      region_set->_regions[cs_index] = _regions[r];
      _regions[r]->set_is_in_collection_set(true);
      cs_index++;
    }
    r++;
  }

  region_set->_inserted = cs_index;
  region_set->_index = 0;
  region_set->_numRegions = cs_index;

}

void ShenandoahHeapRegionSet::choose_empty_regions(ShenandoahHeapRegionSet* region_set) {
  sortDescendingFree();
  int r = 0;
  while(r < _numRegions && _regions[r]->free() > _free_threshold) {
    ShenandoahHeapRegion* region = _regions[r];
    assert(! region->is_humonguous(), "don't reuse occupied humonguous regions");
    region_set->_regions[r] = region;
    r++;
  }

  region_set->_inserted = r;
  region_set->_index = 0;
  region_set->_numRegions = r;
}  

void ShenandoahHeapRegionSet::reclaim_humonguous_regions() {

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (int r = 0; r < _numRegions; r++) {
    // We can immediately reclaim humonguous objects/regions that are no longer reachable.
    ShenandoahHeapRegion* region = _regions[r];
    assert(r == region->region_number(), "we need the regions in original order, not sorted");
    if (region->is_humonguous_start()) {
      oop humonguous_obj = oop(region->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
      if (! heap->isMarkedCurrent(humonguous_obj)) {
        reclaim_humonguous_region_at(r);
      }
    }
  }

}

void ShenandoahHeapRegionSet::reclaim_humonguous_region_at(int r) {
  assert(_regions[r]->is_humonguous_start(), "reclaim regions starting with the first one");
  if (ShenandoahGCVerbose) {
    tty->print_cr("recycling humonguous region:");
    _regions[r]->print();
  }
  _regions[r]->recycle();
  for (int i = r + 1; i < _numRegions; i++) {
    if (_regions[i]->is_humonguous_continuation()) {
      if (ShenandoahGCVerbose) {
        tty->print_cr("recycling humonguous region:");
        _regions[i]->print();
      }
      _regions[i]->recycle();
    } else {
      break;
    }
  }
}

 ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
   while (_index < _inserted) {
     ShenandoahHeapRegion* result = _regions[_index];
     if (result->claim()) {
       Atomic::add(1, &_index);
       if (ShenandoahGCVerbose)
	 tty->print("Claiming region %p\n", result);
       return result;
     }
   }
   return NULL;
 }

  

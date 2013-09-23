#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "memory/resourceArea.hpp"

int compareHeapRegions(ShenandoahHeapRegion** a, ShenandoahHeapRegion** b) {
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

void ShenandoahHeapRegionSet::put(size_t index, ShenandoahHeapRegion* region) {
  _regions[index] = region;
  _inserted++;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get_next() {
  ShenandoahHeapRegion* result = NULL;

  if (_index <= _inserted) 
    result = _regions[_index++];

  return result;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::peek_next() {
  ShenandoahHeapRegion* result = NULL;

  if (_index <= _inserted) 
    result = _regions[_index];

  return result;
}

bool ShenandoahHeapRegionSet::has_next() {
  return _index < _inserted;
}

void ShenandoahHeapRegionSet::sortAscendingGarbage() {
  ResourceMark rm;

  GrowableArray<ShenandoahHeapRegion*>* rs = 
    new GrowableArray<ShenandoahHeapRegion*>();
  for (int i = 0; i < _inserted; i++)
    rs->append(_regions[i]);

  rs->sort(compareHeapRegions);

  int j = _inserted;
  for (int i = 0; i < _inserted; i++) 
    _regions[--j] = rs->at(i);
}
   
void ShenandoahHeapRegionSet::sortDescendingGarbage() {
  ResourceMark rm;

  GrowableArray<ShenandoahHeapRegion*>* rs = 
    new GrowableArray<ShenandoahHeapRegion*>();

  for (int i = 0; i < _inserted; i++)
    rs->append(_regions[i]);

  rs->sort(compareHeapRegions);

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
  region_set->_inserted = 0;
  region_set->_index = 0;

  for (int i = 0; i < max_regions; i++)
    if (_regions[i]->garbage() > _garbage_threshold) 
      region_set->_regions[region_set->_inserted++] = _regions[i];


  tty->print("choose_collection_Set: max_regions = %d regions = %p\n", max_regions);
  for (int i = 0; i < max_regions; i++)
    region_set->_regions[i]->print();
}

void ShenandoahHeapRegionSet::choose_empty_regions(ShenandoahHeapRegionSet* region_set, int max_regions) {
  sortAscendingGarbage();
  region_set->_inserted = 0;
  region_set->_index = 0;

  for (int i = 0; i < max_regions; i++)
      region_set->_regions[region_set->_inserted++] = _regions[i];
}

void ShenandoahHeapRegionSet::choose_collection_set(ShenandoahHeapRegionSet* region_set) {
  sortDescendingGarbage();
  int r = 0;
  while (_regions[r]->garbage() > _garbage_threshold) {
    region_set->_regions[r] = _regions[r];
    r++;
  }

  region_set->_inserted = r;
  region_set->_index = 0;
  region_set->_numRegions = r;

}

void ShenandoahHeapRegionSet::choose_empty_regions(ShenandoahHeapRegionSet* region_set) {
  sortAscendingGarbage();
  int r = 0;
  while(_regions[r]->free() > _free_threshold) {
    region_set->_regions[r] = _regions[r];
    r++;
  }

  region_set->_inserted = r;
  region_set->_index = 0;
  region_set->_numRegions = r;
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

  

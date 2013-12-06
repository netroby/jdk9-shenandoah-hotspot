#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"


class ShenandoahHeapRegionSet : public CHeapObj<mtGC> {
private:
  ShenandoahHeapRegion** _regions;
  // current region to be returned from get_next()
  volatile int _index;
  // last inserted region.
  int _inserted;
  // size of the set.
  int _numRegions;
  size_t _garbage_threshold;
  size_t _free_threshold;
public:
  ShenandoahHeapRegionSet(size_t numRegions) :  
    _index(0),
    _inserted(0),
    _numRegions(numRegions),
    _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, numRegions, mtGC)),
    _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
    _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {}

  void put(size_t i, ShenandoahHeapRegion* region);
  void append(ShenandoahHeapRegion* region);

  ShenandoahHeapRegion* at(uint i) { return _regions[i];}
  size_t length() { return _numRegions;}
  size_t available_regions() { return _inserted - _index;}
  void print();

  bool has_next();
  ShenandoahHeapRegion* get_next();
  ShenandoahHeapRegion* peek_next();
  ShenandoahHeapRegion* claim_next();

  void choose_collection_set(ShenandoahHeapRegionSet* region_set, int max_regions);
  void choose_collection_set(ShenandoahHeapRegionSet* region_set);
  void choose_empty_regions(ShenandoahHeapRegionSet* region_set);

  //  ShenandoahHeapRegion** regions() { return _regions;}
  // Sort from most free to least free.
  void sortDescendingFree();

  // Sort from most garbage to least garbage.
  void sortDescendingGarbage();


};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

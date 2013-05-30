#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"


class ShenandoahHeapRegionSet {
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
    _regions(new ShenandoahHeapRegion*[numRegions]),
    _garbage_threshold(ShenandoahHeapRegion::GarbageThreshold),
    _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2) {}

  void add(ShenandoahHeapRegion* region);

  // Sort from least garbage to most garbage.
  void sortAscendingGarbage();

  // Sort from most garbage to least garbage.
  void sortDescendingGarbage();

  size_t length() { return _numRegions;}
  ShenandoahHeapRegion* at(uint i) { return _regions[i];}
  void print();
  bool has_next();
  ShenandoahHeapRegion* get_next();
  ShenandoahHeapRegion* claim_next();
  void choose_collection_set(ShenandoahHeapRegionSet* region_set, int max_regions);
  void choose_collection_set(ShenandoahHeapRegionSet* region_set);
  void choose_empty_regions(ShenandoahHeapRegionSet* region_set, int max_regions);
  void choose_empty_regions(ShenandoahHeapRegionSet* region_set);
  ShenandoahHeapRegion** regions() { return _regions;}
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

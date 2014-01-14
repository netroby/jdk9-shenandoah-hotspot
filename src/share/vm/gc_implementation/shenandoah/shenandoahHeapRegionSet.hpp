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
  ShenandoahHeapRegionSet(size_t numRegions);

  ShenandoahHeapRegionSet(size_t num_regions, ShenandoahHeapRegion** regions);

  ~ShenandoahHeapRegionSet();

  void put(size_t i, ShenandoahHeapRegion* region);
  void append(ShenandoahHeapRegion* region);

  ShenandoahHeapRegion* at(uint i);
  size_t length();
  size_t available_regions();
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

  // Check for unreachable humonguous regions and reclaim them.
  void reclaim_humonguous_regions();

private:
  void reclaim_humonguous_region_at(int r);

};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

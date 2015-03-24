/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"


class ShenandoahHeapRegionSet : public CHeapObj<mtGC> {
private:
  ShenandoahHeapRegion** _regions;
  // current region to be returned from get_next()
  ShenandoahHeapRegion** _current;
  ShenandoahHeapRegion** _next;

  // last inserted region.
  ShenandoahHeapRegion** _next_free;
  ShenandoahHeapRegion** _concurrent_next_free;

  // Maximum size of the set.
  const size_t _max_regions;

  size_t _garbage_threshold;
  size_t _free_threshold;

  void choose_collection_set(ShenandoahHeapRegion** regions, size_t length);
  void choose_collection_set_min_garbage(ShenandoahHeapRegion** regions, size_t length, size_t min_garbage);
  void choose_free_set(ShenandoahHeapRegion** regions, size_t length);

public:
  ShenandoahHeapRegionSet(size_t max_regions);

  ShenandoahHeapRegionSet(size_t max_regions, ShenandoahHeapRegion** regions, size_t num_regions);

  ~ShenandoahHeapRegionSet();

  void set_garbage_threshold(size_t minimum_garbage) { _garbage_threshold = minimum_garbage;}
  void set_free_threshold(size_t minimum_free) { _free_threshold = minimum_free;}

  /**
   * Appends a region to the set. This is implemented to be concurrency-safe.
   */
  void append(ShenandoahHeapRegion* region);

  void clear();

  size_t length();
  size_t available_regions();
  void print();

  size_t garbage();
  size_t used();
  size_t live_data();
  size_t reclaimed() {return _reclaimed;}

  /**
   * Returns a pointer to the current region.
   */
   ShenandoahHeapRegion* current();

  /**
   * Gets the next region for allocation (from free-list).
   * If multiple threads are competing, one will succeed to
   * increment to the next region, the others will fail and return
   * the region that the succeeding thread got.
   */
  ShenandoahHeapRegion* get_next();

  /**
   * Claims next region for processing. This is implemented to be concurrency-safe.
   */
  ShenandoahHeapRegion* claim_next();

  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set);
  void choose_collection_and_free_sets_min_garbage(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set, size_t min_garbage);

  // Check for unreachable humonguous regions and reclaim them.
  void reclaim_humonguous_regions();

  void set_concurrent_iteration_safe_limits();

private:
  void reclaim_humonguous_region_at(ShenandoahHeapRegion** r);

  ShenandoahHeapRegion** limit_region(ShenandoahHeapRegion** region);
  size_t _reclaimed;

};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

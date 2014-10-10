/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"

class ShenandoahHeap;
class ShenandoahHeuristics;

class ShenandoahCollectorPolicy: public CollectorPolicy {
  ShenandoahHeap* _pgc;
  ShenandoahHeuristics* _heuristics;

public:
  ShenandoahCollectorPolicy();

  virtual ShenandoahCollectorPolicy* as_pgc_policy();

  virtual ShenandoahCollectorPolicy::Name kind();

  BarrierSet::Name barrier_set_name();

  HeapWord* mem_allocate_work(size_t size,
			      bool is_tlab,
			      bool* gc_overhead_limit_was_exceeded);

  HeapWord* satisfy_failed_allocation(size_t size, bool is_tlab);

  void initialize_alignments();

  void post_heap_initialize();

  void record_init_mark_start();
  void record_init_mark_end();
  void record_concurrent_mark_start();
  void record_concurrent_mark_end();
  void record_final_mark_start();
  void record_final_mark_end();
  void record_concurrent_evacuation_start();
  void record_concurrent_evacuation_end();
  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  bool should_start_concurrent_mark(size_t used, size_t capacity);
  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set, 
                                       ShenandoahHeapRegionSet* collection_set,
                                       ShenandoahHeapRegionSet* free_set);

  void print_tracing_info();
};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

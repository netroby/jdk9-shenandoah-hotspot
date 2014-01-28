/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

class ShenandoahHeap;

class ShenandoahCollectorPolicy: public CollectorPolicy {
  ShenandoahHeap* _pgc;

public:
  ShenandoahCollectorPolicy();

  virtual ShenandoahCollectorPolicy* as_pgc_policy();

  virtual ShenandoahCollectorPolicy::Name kind();

  BarrierSet::Name barrier_set_name();

  GenRemSet::Name rem_set_name();

  HeapWord* mem_allocate_work(size_t size,
			      bool is_tlab,
			      bool* gc_overhead_limit_was_exceeded);

  HeapWord* satisfy_failed_allocation(size_t size, bool is_tlab);

  void initialize_alignments();

  void post_heap_initialize();
};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

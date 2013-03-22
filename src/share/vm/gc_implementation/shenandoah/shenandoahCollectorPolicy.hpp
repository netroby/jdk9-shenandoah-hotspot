#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "memory/collectorPolicy.hpp"

class ShenandoahHeap;

class ShenandoahCollectorPolicy: public CollectorPolicy {

  ShenandoahHeap* _pgc;
  uintx min_heap_size;
  size_t region_size;

public:
  ShenandoahCollectorPolicy();

  uint initial_heap_byte_size() { return min_heap_size;}
  uint min_alignment() { return 1;}

  virtual ShenandoahCollectorPolicy* as_pgc_policy() {return this;}

  virtual ShenandoahCollectorPolicy::Name kind() {
    return CollectorPolicy::ShenandoahCollectorPolicyKind;
  }

  BarrierSet::Name barrier_set_name() {
    return BarrierSet::ShenandoahBarrierSet;
  }

  GenRemSet::Name rem_set_name() {
    return GenRemSet::Other;
  }

  HeapWord* mem_allocate_work(size_t size,
			      bool is_tlab,
			      bool* gc_overhead_limit_was_exceeded) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
  }

  HeapWord* satisfy_failed_allocation(size_t size, bool is_tlab) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

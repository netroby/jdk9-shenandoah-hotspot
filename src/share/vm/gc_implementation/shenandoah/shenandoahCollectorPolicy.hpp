#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "memory/collectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

class ShenandoahHeap;



class ShenandoahCollectorPolicy: public CollectorPolicy {
  ShenandoahHeap* _pgc;

public:
  ShenandoahCollectorPolicy();

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

  void initialize_flags() {
    set_min_alignment(ShenandoahHeapRegion::RegionSizeBytes);
    set_max_alignment(ShenandoahHeapRegion::RegionSizeBytes);
  }

  void initialize_all() {
    initialize_flags();
    initialize_size_info();
  }

};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

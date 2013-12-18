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

  void initialize_alignments() {
    _space_alignment = Arguments::conservative_max_heap_alignment();
    _heap_alignment = Arguments::conservative_max_heap_alignment();
  }

  void post_heap_initialize() {
    // Nothing to do here (yet).
  }
};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

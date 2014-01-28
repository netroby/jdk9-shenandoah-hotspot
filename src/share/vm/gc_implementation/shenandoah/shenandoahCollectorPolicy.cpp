/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"



ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() {
  initialize_all();
}


ShenandoahCollectorPolicy* ShenandoahCollectorPolicy::as_pgc_policy() {
  return this;
}

ShenandoahCollectorPolicy::Name ShenandoahCollectorPolicy::kind() {
  return CollectorPolicy::ShenandoahCollectorPolicyKind;
}

BarrierSet::Name ShenandoahCollectorPolicy::barrier_set_name() {
  return BarrierSet::ShenandoahBarrierSet;
}

GenRemSet::Name ShenandoahCollectorPolicy::rem_set_name() {
  return GenRemSet::Other;
}

HeapWord* ShenandoahCollectorPolicy::mem_allocate_work(size_t size,
                                                       bool is_tlab,
                                                       bool* gc_overhead_limit_was_exceeded) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

HeapWord* ShenandoahCollectorPolicy::satisfy_failed_allocation(size_t size, bool is_tlab) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

void ShenandoahCollectorPolicy::initialize_alignments() {
  _space_alignment = Arguments::conservative_max_heap_alignment();
  _heap_alignment = Arguments::conservative_max_heap_alignment();
}

void ShenandoahCollectorPolicy::post_heap_initialize() {
  // Nothing to do here (yet).
}

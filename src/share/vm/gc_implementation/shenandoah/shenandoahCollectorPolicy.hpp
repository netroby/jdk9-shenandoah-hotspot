/*
  Copyright 2014 Red Hat, Inc. and/or its affiliates.
*/
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "utilities/numberSeq.hpp"


class ShenandoahHeap;
class ShenandoahHeuristics;

class ShenandoahCollectorPolicy: public CollectorPolicy {

public:
  enum TimingPhase {
    init_mark,
    final_mark,
    rescan_roots,
    drain_satb,
    drain_overflow,
    drain_queues,
    weakrefs,
    prepare_evac,
    init_evac,

    final_evac,
    final_uprefs,
    update_roots,
    recycle_regions,
    reset_bitmaps,
    resize_tlabs,
    full_gc,
    conc_mark,
    conc_evac,

    _num_phases
  };

private:
  struct TimingData {
    NumberSeq _ms;
    double _start;
    size_t _count;
  };

private:
  TimingData _timing_data[_num_phases];
  const char* _phase_names[_num_phases];

  size_t _user_requested_gcs;
  size_t _allocation_failure_gcs;

  ShenandoahHeap* _pgc;
  ShenandoahHeuristics* _heuristics;
  ShenandoahTracer* _tracer;
  STWGCTimer* _stw_timer;
  ConcurrentGCTimer* _conc_timer;

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

  void record_phase_start(TimingPhase phase);
  void record_phase_end(TimingPhase phase);

  void record_user_requested_gc();
  void record_allocation_failure_gc();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  bool should_start_concurrent_mark(size_t used, size_t capacity);
  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set, 
                                       ShenandoahHeapRegionSet* collection_set,
                                       ShenandoahHeapRegionSet* free_set);

  void print_tracing_info();
  GCTimer* conc_timer(){return _conc_timer;}
  GCTimer* stw_timer() {return _stw_timer;}

private:
  void print_summary_sd(const char* str, uint indent, const NumberSeq* seq);
};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

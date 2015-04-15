/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHMARKCOMPACT_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHMARKCOMPACT_HPP

#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/sharedOverflowMarkQueue.hpp"
#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"

typedef Padded<OopTaskQueue> ObjToScanQueue;
typedef GenericTaskQueueSet<ObjToScanQueue, mtGC> ObjToScanQueueSet;

class HeapWord;
class ShenandoahHeap;

class ShenandoahMarkCompactBarrierSet : public ShenandoahBarrierSet {
  //ShenanodoahMarkCompactBarrierSet() {}
  oop resolve_oop(oop src) {
    return src;
  }
  oop maybe_resolve_oop(oop src) {
    return src;
  }
};

/**
 * This implements full-GC (e.g. when invoking System.gc() ) using a
 * mark-compact algorithm. It's implemented in four phases:
 *
 * 1. Mark all live objects of the heap by traversing objects starting at GC roots.
 * 2. Calculate the new location of each live object. This is done by sequentially scanning
 *    the heap, keeping track of a next-location-pointer, which is then written to each
 *    object's brooks ptr field.
 * 3. Update all references. This is implemented by another scan of the heap, and updates
 *    all references in live objects by what's stored in the target object's brooks ptr.
 * 3. Compact the heap by copying all live objects to their new location.
 */
class ShenandoahMarkCompact {

private:
  ShenandoahHeap* _heap;
  ShenandoahMarkCompactBarrierSet _barrier_set;
  uint _max_worker_id;
  ObjToScanQueueSet* _task_queues;
  SharedOverflowMarkQueue* _overflow_queue;
  int _seed;

public:
  ShenandoahMarkCompact();
  void do_mark_compact();
  void initialize();
  void add_task(oop obj, int q);
  oop  pop_task(int q);
  oop  get_task(int q);
  oop  steal_task(int q);
private:

  void phase1_mark_heap();
  void phase2_calculate_target_addresses();
  void phase3_update_references();
  void phase4_compact_objects();
  void finish_compaction(HeapWord* last_addr);

#if TASKQUEUE_STATS
  static void print_taskqueue_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_taskqueue_stats(outputStream* const st = gclog_or_tty) const;
  void reset_taskqueue_stats();
#endif // TASKQUEUE_STATS


};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHMARKCOMPACT_HPP

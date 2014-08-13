/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"
#include "gc_implementation/shenandoah/sharedOverflowMarkQueue.hpp"

typedef Padded<OopTaskQueue> SCMObjToScanQueue;
typedef GenericTaskQueueSet<SCMObjToScanQueue, mtGC> SCMObjToScanQueueSet;

class ShenandoahConcurrentMark: public CHeapObj<mtGC> {

private:
  // The per-worker-thread work queues
  SCMObjToScanQueueSet* _task_queues;

  // The shared mark stack that is used in case of overflow.
  SharedOverflowMarkQueue* _overflow_queue;

  bool                    _aborted;       

public:
  //  ShenandoahConcurrentMark();

  void scanRootRegions();
  void markFromRoots();
  void checkpointRootsFinal();
  void finishMarkFromRoots(bool full_gc = false);
  void drain_satb_buffers(uint worker_id, bool remark = false);
  bool drain_one_satb_buffer(uint worker_id);
  bool has_aborted() {return _aborted;}
  SharedOverflowMarkQueue* overflow_queue();
  void weakRefsWork(bool clear_soft_refs, int worker_id);

  bool try_queue(uint worker_id, ExtendedOopClosure* cl);
  bool try_to_steal(uint worker_id, ExtendedOopClosure* cl, int *seed);
  bool try_overflow_queue(uint worker_id, ExtendedOopClosure *cl);
  bool try_draining_an_satb_buffer(uint worker_id);
  void traverse_object(ExtendedOopClosure *cl, oop obj);

  void addTask(oop obj, int worker_id);
  //  oop popTask(int worker_id);

  uint _max_worker_id;
  ParallelTaskTerminator* _terminator;
  ParallelTaskTerminator* terminator() { return _terminator;}
  SCMObjToScanQueueSet* task_queues() { return _task_queues;}

  // We need to do this later when the heap is already created.
  void initialize();

#if TASKQUEUE_STATS
  static void print_taskqueue_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_taskqueue_stats(outputStream* const st = gclog_or_tty) const;
  void reset_taskqueue_stats();
#endif // TASKQUEUE_STATS

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

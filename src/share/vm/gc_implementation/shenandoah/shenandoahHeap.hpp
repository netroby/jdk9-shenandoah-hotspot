/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc_implementation/shenandoah/shenandoahAllocRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"

#include "gc_implementation/g1/concurrentMark.hpp"


#include "memory/barrierSet.hpp"
#include "memory/sharedHeap.hpp"
#include "memory/space.inline.hpp"
#include "oops/oop.hpp"
#include "oops/markOop.hpp"


class SpaceClosure;
class EvacuationAllocator;

class ShenandoahIsAliveClosure: public BoolObjectClosure {

public:
  bool do_object_b(oop obj);
};


class ShenandoahHeapRegionClosure : public StackObj {
  bool _complete;
  void incomplete() {_complete = false;}

public:
  ShenandoahHeapRegionClosure(): _complete(true) {}

  // typically called on each region until it returns true;
  virtual bool doHeapRegion(ShenandoahHeapRegion* r) = 0;

  bool complete() { return _complete;}
};

// A "ShenandoahHeap" is an implementation of a java heap for HotSpot.
// It uses a new pauseless GC algorithm based on Brooks pointers.
// Derived from G1

// 
// CollectedHeap  
//    SharedHeap
//      ShenandoahHeap

class ShenandoahHeap : public SharedHeap {

private:

  static ShenandoahHeap* _pgc;
  ShenandoahCollectorPolicy* _shenandoah_policy;
  VirtualSpace _storage;
  ShenandoahHeapRegion* _first_region;
  HeapWord* _first_region_bottom;
  // Ordered array of regions  (name confusing with _regions)
  ShenandoahHeapRegion** _ordered_regions;

  // Sortable array of regions
  ShenandoahHeapRegionSet* _free_regions;
  ShenandoahHeapRegionSet* _collection_set;
  ShenandoahHeapRegion* _currentAllocationRegion;
  ShenandoahConcurrentMark* _scm;

  ShenandoahConcurrentThread* _concurrent_gc_thread;

  size_t _num_regions;
  size_t _max_regions;
  size_t _initialSize;
#ifndef NDEBUG
  uint _numAllocs;
#endif
  size_t _default_gclab_size;
  WorkGangBarrierSync barrierSync;
  int _max_workers;
  volatile size_t _used;

  CMBitMap _mark_bit_map0;
  CMBitMap* _next_mark_bit_map;

  CMBitMap _mark_bit_map1;
  CMBitMap* _prev_mark_bit_map;

public:
  size_t _bytesAllocSinceCM;

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);
  HeapWord* allocate_new_tlab(size_t word_size);
  void retire_tlab_at(HeapWord* start);
  HeapWord* allocate_new_gclab(size_t word_size);

  HeapWord* allocate_memory(size_t word_size);

  bool find_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions);
  bool allocate_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions);

  HeapWord* allocate_new_gclab() { 
    return allocate_new_gclab(_default_gclab_size);
  }

  // For now we are ignoring eden.
  inline bool should_alloc_in_eden(size_t size) { return false;}
  void print_on(outputStream* st) const ;

  ShenandoahHeap::Name kind() const {
    return CollectedHeap::ShenandoahHeap;
  }
  
  static ShenandoahHeap* heap();

  ShenandoahCollectorPolicy *shenandoahPolicy() { return _shenandoah_policy;}

  jint initialize();
  void post_initialize();
  size_t capacity() const;
  size_t used() const;
  bool is_maximal_no_gc() const;
  size_t max_capacity() const;
  virtual bool is_in(const void* p) const;
  bool is_in_partial_collection(const void* p);
  bool is_scavengable(const void* addr);
  virtual HeapWord* mem_allocate(size_t size, bool* what);
  HeapWord* mem_allocate_locked(size_t size, bool* what);
  virtual size_t unsafe_max_alloc();
  bool can_elide_tlab_store_barriers() const;
  bool can_elide_initializing_store_barrier(oop new_obj);
  bool card_mark_must_follow_store() const;
  bool supports_heap_inspection() const;
  void collect(GCCause::Cause);
  void do_full_collection(bool clear_all_soft_refs);
  AdaptiveSizePolicy* size_policy();
  ShenandoahCollectorPolicy* collector_policy() const;
  void oop_iterate(ExtendedOopClosure* cl, bool skip_dirty_regions,
                   bool skip_unreachable_objects);
  void oop_iterate(ExtendedOopClosure* cl) {
    oop_iterate(cl, false, false);
  }

  void roots_iterate(ExtendedOopClosure* cl);

  
  void object_iterate(ObjectClosure* cl);
  void object_iterate_no_from_space(ObjectClosure* cl);
  void safe_object_iterate(ObjectClosure* cl);

  HeapWord* block_start(const void* addr) const;
  size_t block_size(const HeapWord* addr) const;
  bool block_is_obj(const HeapWord* addr) const;
  jlong millis_since_last_gc();
  void prepare_for_verify();
  void print_gc_threads_on(outputStream* st) const;
  void gc_threads_do(ThreadClosure* tcl) const;
  void print_tracing_info() const;
  void verify(bool silent,  VerifyOption vo);
  bool supports_tlab_allocation() const;
  virtual size_t tlab_capacity(Thread *thr) const;
  void oop_iterate(MemRegion mr, ExtendedOopClosure* ecl);
  void object_iterate_since_last_GC(ObjectClosure* cl);
  void space_iterate(SpaceClosure* scl);
  virtual size_t unsafe_max_tlab_alloc(Thread *thread) const;

  HeapWord* tlab_post_allocation_setup(HeapWord* obj, bool new_obj);

  uint oop_extra_words();

#ifndef CC_INTERP
  void compile_prepare_oop(MacroAssembler* masm, Register obj = rax);
#endif

  Space* space_containing(const void* oop) const;
  void gc_prologue(bool b);
  void gc_epilogue(bool b);

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions = false, bool skip_humonguous_continuation = false) const;
  ShenandoahHeapRegion* heap_region_containing(const void* addr) const;  

/**
 * Maybe we need that at some point...

  oop* resolve_oop_ptr(oop* p);

  oop oop_containing_oop_ptr(oop* p);

*/

  void temp();

  volatile unsigned int _concurrent_mark_in_progress;

  volatile unsigned int _evacuation_in_progress;
  volatile bool _update_references_in_progress;
  volatile bool _waiting_for_jni_before_gc;

  bool should_start_concurrent_marking();
  void start_concurrent_marking();
  void stop_concurrent_marking();
  ShenandoahConcurrentMark* concurrentMark() { return _scm;}
  size_t bump_object_age(HeapWord* start, HeapWord* end);
  bool mark_current(oop obj) const;
  bool mark_current_no_checks(oop obj) const;
  bool isMarkedCurrent(oop obj) const;
  ReferenceProcessor* _ref_processor_cm;
  bool is_marked_prev(oop obj) const;

  bool is_obj_ill(const oop obj) {
    return !isMarkedCurrent(obj);
  }

  void reset_mark_bitmap();

  virtual void post_allocation_collector_specific_setup(HeapWord* obj);

  void mark_object_live(oop obj, bool enqueue);

  // Prepares unmarked root objects by marking them and putting
  // them into the marking task queue.
  void prepare_unmarked_root_objs();

  void prepare_for_concurrent_evacuation();
  void do_evacuation();
  void parallel_evacuate();

  void initialize_brooks_ptr(HeapWord* brooks_ptr, HeapWord* object, bool new_obj = true);

  oop maybe_update_oop_ref(oop* p);
  void evacuate_region(ShenandoahHeapRegion* from_region, ShenandoahHeapRegion* to_region);
  void parallel_evacuate_region(ShenandoahHeapRegion* from_region,
				ShenandoahAllocRegion *alloc_region);
  void verify_evacuated_region(ShenandoahHeapRegion* from_region);

  void print_heap_regions(outputStream* st = tty) const;

  void print_all_refs(const char* prefix);

  oop  evacuate_object(oop src, EvacuationAllocator* allocator);
  bool is_in_collection_set(oop* p) {
    return heap_region_containing(p)->is_in_collection_set();
  }
  
  void copy_object(oop p, HeapWord* s);
  void verify_copy(oop p, oop c);
  //  void assign_brooks_pointer(oop p, HeapWord* filler, HeapWord* copy);
  void verify_heap_after_marking();
  void verify_heap_after_evacuation();
  void verify_heap_after_update_refs();

  // This is here to get access to the otherwise protected method in CollectedHeap.
  static HeapWord* allocate_from_tlab_work(Thread* thread, size_t size);

  static ByteSize ordered_regions_offset() { return byte_offset_of(ShenandoahHeap, _ordered_regions); }
  static ByteSize first_region_bottom_offset() { return byte_offset_of(ShenandoahHeap, _first_region_bottom); }
  static address evacuation_in_progress_addr() {
    return (address) (& ShenandoahHeap::heap()->_evacuation_in_progress);
  }

  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);

  int ensure_new_regions(int num_new_regions);

  void set_evacuation_in_progress(bool in_progress);
  bool is_evacuation_in_progress();

  bool is_update_references_in_progress();
  void set_update_references_in_progress(bool update_refs_in_progress);

  void set_waiting_for_jni_before_gc(bool wait_for_jni);
  bool is_waiting_for_jni_before_gc();


  ReferenceProcessor* ref_processor_cm() { return _ref_processor_cm;}	
  virtual void ref_processing_init();
  ShenandoahIsAliveClosure isAlive;
  void evacuate_and_update_roots();
  void prepare_for_update_references();

  void update_references();

  ShenandoahHeapRegionSet* free_regions();

  void update_roots();

private:

  bool grow_heap_by();

  void verify_evacuation(ShenandoahHeapRegion* from_region);
  void set_concurrent_mark_in_progress(bool in_progress);


private:
  bool concurrent_mark_in_progress();
  void verify_live();
  void verify_liveness_after_concurrent_mark();

  HeapWord* allocate_memory_work(size_t word_size);
  HeapWord* allocate_large_memory(size_t word_size);
  ShenandoahHeapRegion* check_skip_humonguous(ShenandoahHeapRegion* region);
  ShenandoahHeapRegion* get_next_region_skip_humonguous();
  ShenandoahHeapRegion* get_current_region_skip_humonguous();
  ShenandoahHeapRegion* check_grow_heap(ShenandoahHeapRegion* current);
  ShenandoahHeapRegion* get_next_region_for_allocation();
  ShenandoahHeapRegion* get_current_region_for_allocation();

  void set_from_region_protection(bool protect);
};



// Mark the object and add it to the queue to be scanned
class ShenandoahMarkObjsClosure : public ObjectClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
public: 
  ShenandoahMarkObjsClosure(uint worker_id);
  void do_object(oop p);
};  


// Walks over all the objects in the generation updating any
// references to from space.

class ShenandoahMarkRefsClosure : public ExtendedOopClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
  ShenandoahMarkObjsClosure _mark_objs;

public: 
  ShenandoahMarkRefsClosure(uint worker_id);

  void do_oop_work(oop* p);
  void do_oop(narrowOop* p);
  void do_oop(oop* p);
};

class ShenandoahMarkRefsNoUpdateClosure : public ExtendedOopClosure {
  uint _worker_id;
  ShenandoahHeap* _heap;
  ShenandoahMarkObjsClosure _mark_objs;
  
public:
  ShenandoahMarkRefsNoUpdateClosure(uint worker_id);

  void do_oop_work(oop* p);
  void do_oop(narrowOop* p);
  void do_oop(oop* p);

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc_implementation/shenandoah/shenandoahAllocRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"


#include "memory/barrierSet.hpp"
#include "memory/sharedHeap.hpp"
#include "memory/space.inline.hpp"
#include "oops/oop.hpp"
#include "oops/markOop.hpp"

#define MAX_EPOCH 14

class SpaceClosure;

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

class ShenandoahHeap : public SharedHeap {

private:
  static ShenandoahHeap* _pgc;
  ShenandoahCollectorPolicy* _pgc_policy;
  VirtualSpace _storage;
  ShenandoahHeapRegion* _current_region;
  ShenandoahHeapRegion* _first_region;
  // Ordered array of regions  (name confusing with _regions)
  ShenandoahHeapRegion** _ordered_regions;

  // Sortable array of regions
  ShenandoahHeapRegionSet* _regions;
  ShenandoahHeapRegionSet* _free_regions;
  ShenandoahHeapRegionSet* _collection_set;
  ShenandoahHeapRegion* _currentAllocationRegion;
  ShenandoahConcurrentMark* _scm;

  size_t _numRegions;
  size_t _initialSize;
#ifndef NDEBUG
  uint _numAllocs;
#endif
  uint _epoch;
  size_t _bytesAllocSinceCM;
  size_t _default_gclab_size;
  WorkGangBarrierSync barrierSync;
  int _max_workers;

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);
  HeapWord* allocate_new_tlab(size_t word_size);
  HeapWord* allocate_new_gclab(size_t word_size);
  HeapWord* allocate_new_gclab() { 
    return allocate_new_gclab(_default_gclab_size);
  }

  uint getEpoch() {return _epoch;}

  // For now we are ignoring eden.
  inline bool should_alloc_in_eden(size_t size) { return false;}
  void print_on(outputStream* st) const ;

  ShenandoahHeap::Name kind() const {
    return CollectedHeap::ShenandoahHeap;
  }
  
  static ShenandoahHeap* heap();

  ShenandoahCollectorPolicy *shenandoahPolicy() { return _pgc_policy;}
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
  void object_iterate(ObjectClosure* cl);
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
  virtual bool supports_tlab_allocation() const {return false;}
  virtual size_t tlab_capacity(Thread *thr) const;
  void oop_iterate(MemRegion mr, ExtendedOopClosure* ecl);
  void object_iterate_since_last_GC(ObjectClosure* cl);
  void space_iterate(SpaceClosure* scl);
  virtual size_t unsafe_max_tlab_alloc(Thread *thread) const;

  Space* space_containing(const void* oop) const;
  void gc_prologue(bool b);
  void gc_epilogue(bool b);

  size_t used_in_bytes() { return used() * HeapWordSize;}
  size_t capacity_in_bytes() { return capacity() * HeapWordSize;}

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions = false) const;
  template<class T> ShenandoahHeapRegion* heap_region_containing(const T addr) const;  

  bool is_in_reserved(void* p);

  void temp();

  volatile unsigned int _concurrent_mark_in_progress;

  bool should_start_concurrent_marking();
  void start_concurrent_marking();
  void stop_concurrent_marking();
  ShenandoahConcurrentMark* concurrentMark() { return _scm;}
  size_t bump_object_age(HeapWord* start, HeapWord* end);
  void mark_current(oop obj) const;
  bool isMarkedPrev(oop obj) const;
  bool isMarkedCurrent(oop obj) const;
  bool isMarked(oop obj)  { return isMarkedPrev(obj) || isMarkedCurrent(obj);}
  bool is_obj_ill(oop obj) const { return isMarkedPrev(obj);}
  virtual void post_allocation_collector_specific_setup(HeapWord* obj);

  void mark_object_live(oop obj, bool enqueue);

  // Prepares unmarked root objects by marking them and putting
  // them into the marking task queue.
  void prepare_unmarked_root_objs();

  void parallel_evacuate();

  void initialize_brooks_ptr(HeapWord* brooks_ptr, HeapWord* object);
  void set_brooks_ptr(HeapWord* brooks_ptr, HeapWord* object);

  void maybe_update_oop_ref(oop* p);
  void evacuate_region(ShenandoahHeapRegion* from_region, ShenandoahHeapRegion* to_region);
  void parallel_evacuate_region(ShenandoahHeapRegion* from_region,
				ShenandoahAllocRegion alloc_region);
  void verify_evacuated_region(ShenandoahHeapRegion* from_region);

  void update_current_region();
  ShenandoahHeapRegion* current_region() { return _current_region;}

private:

  void verify_evacuation(ShenandoahHeapRegion* from_region);
  bool set_concurrent_mark_in_progress(bool in_progress);
  bool concurrent_mark_in_progress();
  void verify_live();
  void verify_liveness_after_concurrent_mark();
  void mark();



  
};

class ShenandoahMarkRefsClosure : public OopsInGenClosure {
  uint _epoch;
  uint _worker_id;
  ShenandoahHeap* _heap;

public: 
  ShenandoahMarkRefsClosure(uint e, uint worker_id);

  void do_oop_work(oop* p);
  void do_oop(narrowOop* p);
  void do_oop(oop* p);
};
  
class ShenandoahMarkObjsClosure : public ObjectClosure {
  uint _epoch;
  uint _worker_id;
public: 
  ShenandoahMarkObjsClosure(uint e, uint worker_id);

  void do_object(oop p);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

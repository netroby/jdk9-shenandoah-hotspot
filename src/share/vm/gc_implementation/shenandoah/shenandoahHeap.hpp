#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "memory/barrierSet.hpp"
#include "memory/sharedHeap.hpp"
#include "memory/space.inline.hpp"
#include "oops/oop.hpp"
#include "oops/markOop.hpp"

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
  ShenandoahBarrierSet* _pgc_barrierSet;
  ShenandoahHeapRegion* firstRegion;
  ShenandoahHeapRegion* currentRegion;

  size_t numRegions;
  size_t initialSize;

  ShenandoahConcurrentThread* _sct;
  uint epoch;
  

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);
  HeapWord* allocate_new_tlab(size_t word_size);

  // For now we are ignoring eden.
  inline bool should_alloc_in_eden(size_t size) { return false;}
  void print_on(outputStream* st) const ;
public:
  
  ShenandoahHeap::Name kind() const {
    return CollectedHeap::ShenandoahHeap;
  }
  
  static ShenandoahHeap* heap();

 ShenandoahCollectorPolicy *shenandoahPolicy() { return _pgc_policy;}

  void nyi() const;
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
  CollectorPolicy* collector_policy() const;
  void oop_iterate(ExtendedOopClosure* cl );
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

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk) const;
  template<class T> inline ShenandoahHeapRegion* heap_region_containing(const T addr) const;  

  bool is_in_reserved(void* p);

  void mark(HeapWord* addr) {
    markOop m = (markOop) addr;
    m->set_age(epoch);
  }

  bool isMarked(HeapWord* addr) {
    markOop m = (markOop) addr;
    uint age = m->age();
    return age >= (epoch - 1);
  }

};


#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAP_HPP

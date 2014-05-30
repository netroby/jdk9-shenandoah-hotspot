/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"

#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacuation.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "utilities/copy.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "runtime/atomic.inline.hpp"

#define __ masm->

ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

void printHeapLocations(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur++) {
    //    tty->print("%p : %p \n", cur);
  }
}

void printHeapObjects(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    oop(cur)->print();
    printHeapLocations(cur, cur + oop(cur)->size());
  }
}

class PrintHeapRegionsClosure : public ShenandoahHeapRegionClosure {
private:
  outputStream* _st;
public:
  PrintHeapRegionsClosure() : _st(tty) {}
  PrintHeapRegionsClosure(outputStream* st) : _st(st) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->print(_st);
    return false;
  }
};

class PrintHeapObjectsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x\n", 
	       r->region_number(), r->top(), r->used(), r->free());
    
    printHeapObjects(r->bottom(), r->top());
    return false;
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();
  if (ShenandoahGCVerbose) 
    tty->print("init_byte_size = %d,%x  max_byte_size = %d,%x\n", 
	     init_byte_size, init_byte_size, max_byte_size, max_byte_size);

  Universe::check_alignment(max_byte_size,  
			    ShenandoahHeapRegion::RegionSizeBytes, 
			    "shenandoah heap");
  Universe::check_alignment(init_byte_size, 
			    ShenandoahHeapRegion::RegionSizeBytes, 
			    "shenandoah heap");

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
						 Arguments::conservative_max_heap_alignment());
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*) (heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet());
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);
  _storage.initialize(pgc_rs, init_byte_size);
  if (ShenandoahGCVerbose) {
    tty->print("Calling initialize on reserved space base = %p end = %p\n", 
	       pgc_rs.base(), pgc_rs.base() + pgc_rs.size());
  }

  _num_regions = init_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  _max_regions = max_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  _ordered_regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _max_regions, mtGC); 
  for (size_t i = 0; i < _max_regions; i++) {
    _ordered_regions[i] = NULL;
  }

  _initialSize = _num_regions * ShenandoahHeapRegion::RegionSizeBytes;
  size_t regionSizeWords = ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
  assert(init_byte_size == _initialSize, "tautology");
  _free_regions = new ShenandoahHeapRegionSet(_max_regions);
  _collection_set = new ShenandoahHeapRegionSet(_max_regions);

  for (size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
    current->initialize((HeapWord*) pgc_rs.base() + 
			regionSizeWords * i, regionSizeWords, i);
    _free_regions->put(i, current);
    _ordered_regions[i] = current;
  }
  _first_region = _ordered_regions[0];
  _first_region_bottom = _first_region->bottom();

  _numAllocs = 0;

  if (ShenandoahGCVerbose) {
    tty->print("All Regions\n");
    print_heap_regions();
    tty->print("Free Regions\n");
    _free_regions->print();
  }

  _current_region = NULL;
  cas_update_current_region(NULL);

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);
  if (!_mark_bit_map.allocate(heap_rs)) {
    fatal("Failed to allocate CM bit map");
    return JNI_ENOMEM;
  }
  _next_mark_bit_map = &_mark_bit_map;
  reset_mark_bitmap();

  // TODO: Implement swapping of mark bitmaps.

  _concurrent_gc_thread = new ShenandoahConcurrentThread();
  _concurrent_gc_thread->start();
  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) : 
  SharedHeap(policy),
  _shenandoah_policy(policy), 
  _concurrent_mark_in_progress(false),
  _evacuation_in_progress(false),
  _waiting_for_jni_before_gc(false),
  _free_regions(NULL),
  _collection_set(NULL),
  _bytesAllocSinceCM(0),
  _max_workers((int) MAX2((uint)ParallelGCThreads, 1U)),
  _mark_bit_map(log2_intptr(MinObjAlignment)),
  _default_gclab_size(1024){
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
  _used = 0;
}

void ShenandoahHeap::reset_mark_bitmap() {
  _next_mark_bit_map->clearAll();
}



void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("Shenandoah Heap");
  st->print(" total = " SIZE_FORMAT " K, used " SIZE_FORMAT " K ", capacity()/ K, used() /K);
  st->print("Region size = " SIZE_FORMAT "K, ", ShenandoahHeapRegion::RegionSizeBytes / K);
  PrintHeapRegionsClosure cl(st);
  heap_region_iterate(&cl);
}

void ShenandoahHeap::post_initialize() {
  _scm->initialize(workers());
}

class CalculateUsedRegionClosure : public ShenandoahHeapRegionClosure {
  size_t sum;
public:

  CalculateUsedRegionClosure() {
    sum = 0;
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    sum = sum + r->used();
    return false;
  }

  size_t getResult() { return sum;}
};


  
size_t ShenandoahHeap::used() const {
  return _used;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  Atomic::add_ptr(bytes, &_used);
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(_used - bytes >= 0, "never decrease heap size by more than we've left");
  Atomic::add_ptr(-bytes, &_used);
}

size_t ShenandoahHeap::capacity() const {
  return _num_regions * ShenandoahHeapRegion::RegionSizeBytes;

}

bool ShenandoahHeap::is_maximal_no_gc() const {
  Unimplemented();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return _max_regions * ShenandoahHeapRegion::RegionSizeBytes;
}

class IsInRegionClosure : public ShenandoahHeapRegionClosure {
  const void* _p;
  bool _result;
public:

  IsInRegionClosure(const void* p) {
    _p = p;
    _result = false;
  }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_in(_p)) {
      _result = true;
      return true;
    }
    return false;
  }

  bool result() { return _result;}
};

bool ShenandoahHeap::is_in(const void* p) const {
  //  IsInRegionClosure isIn(p);
  //  heap_region_iterate(&isIn);
  //  bool result = isIn.result();
  
  //  return isIn.result();
  HeapWord* first_region_bottom = _first_region->bottom();
  HeapWord* last_region_end = first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * _num_regions;
  return p > _first_region_bottom && p < last_region_end;
}

bool ShenandoahHeap::is_in_partial_collection(const void* p ) {
  Unimplemented();
  return false;
}  

bool  ShenandoahHeap::is_scavengable(const void* p) {
  //  nyi();
  //  return false;
  return true;
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size) {
  HeapWord* result = allocate_memory_gclab(word_size);

  if (result != NULL) {
    if (_concurrent_mark_in_progress) {
      // We mark the whole tlab here, this way we avoid marking every single
      // allocated object.
      _next_mark_bit_map->parMarkRange(MemRegion(result, word_size));
    }
    assert(! heap_region_containing(result)->is_in_collection_set(), "Never allocate in dirty region");
    _bytesAllocSinceCM += word_size * HeapWordSize;
    heap_region_containing(result)->increase_active_tlab_count();

#ifdef ASSERT
    if (ShenandoahTraceTLabs)
      tty->print("allocating new tlab of size %d at addr %p\n", word_size, result);
#endif

  }
  return result;
}

void ShenandoahHeap::retire_tlab_at(HeapWord* start) {
#ifdef ASSERT
  if (ShenandoahTraceTLabs)
    tty->print_cr("retiring tlab at: %p", start);
#endif 
  heap_region_containing(start)->decrease_active_tlab_count();
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
  HeapWord* result = allocate_memory_gclab(word_size);
  assert(! heap_region_containing(result)->is_in_collection_set(), "Never allocate in dirty region");
  if (result != NULL) {
    if (ShenandoahGCVerbose)
      tty->print("allocating new gclab of size %d at addr %p\n", word_size, result);
  }
  return result;
}
  

ShenandoahHeap* ShenandoahHeap::heap() {
  assert(_pgc != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(_pgc->kind() == CollectedHeap::ShenandoahHeap, "not a shenandoah heap");
  return _pgc;
}

class VM_ShenandoahVerifyHeap: public VM_GC_Operation {
public:
  VM_ShenandoahVerifyHeap(unsigned int gc_count_before,
                   unsigned int full_gc_count_before,
                   GCCause::Cause cause)
    : VM_GC_Operation(gc_count_before, cause, full_gc_count_before) { }
  virtual VMOp_Type type() const { return VMOp_G1CollectFull; }
  virtual void doit() {
    if (ShenandoahGCVerbose)
      tty->print_cr("verifying heap");
     Universe::heap()->prepare_for_verify();
     Universe::verify();
  }
  virtual const char* name() const {
    return "Shenandoah verify trigger";
  }
};

class FindEmptyRegionClosure: public ShenandoahHeapRegionClosure {
  ShenandoahHeapRegion* _result;
  size_t _required_size;
public:

  FindEmptyRegionClosure(size_t required_size) : _required_size(required_size) {
    _result = NULL;
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if ((! r->is_in_collection_set()) && r->free() >= _required_size) {
      _result = r;
      return true;
    }
    return false;
  }
  ShenandoahHeapRegion* result() { return _result;}

};

ShenandoahHeapRegion* ShenandoahHeap::cas_update_current_region(ShenandoahHeapRegion* expected) {
  if (expected != NULL) {
    expected->set_is_current_allocation_region(false);
    expected->clearClaim();
  }
  if (_free_regions->has_next()) {
    ShenandoahHeapRegion* next = _free_regions->peek_next();
    assert(! next->is_in_collection_set(), "Never use targetted regions for allocations.");
    ShenandoahHeapRegion* previous = (ShenandoahHeapRegion*) Atomic::cmpxchg_ptr(next, &_current_region, expected);
    assert(! _current_region->is_humonguous(), "never get humonguous allocation region");
    guarantee(! _current_region->is_in_collection_set(), "Never use targetted regions for allocations.");
    if (previous == expected) {
      // Advance the region set.
      _free_regions->get_next();
      next->set_is_current_allocation_region(true);
      return next;
    } else {
      next->clearClaim(); // peek_next claimed it, unclaim it here, otherwise it stays forever.
      // If the above CAS fails, we want the caller to get the _current_region that the other thread
      // CAS'ed.
      return previous;
    }
  } else {
    return NULL;
  }

}

HeapWord* ShenandoahHeap::allocate_memory_gclab(size_t word_size) {
  MutexLockerEx ml(ShenandoahHeap_lock, true);
  {
    return allocate_memory_work(word_size);
  }
}

HeapWord* ShenandoahHeap::allocate_memory_work(size_t word_size) {

  ShenandoahHeapRegion* my_current_region = _current_region;
#ifdef ASSERT
  if (my_current_region->is_in_collection_set()) {
    print_heap_regions();
  }
#endif
  assert(! my_current_region->is_in_collection_set(), "never get targetted regions in free-lists");
  assert(! my_current_region->is_humonguous(), "never attempt to allocate from humonguous object regions");
  if (my_current_region == NULL) {
    my_current_region = cas_update_current_region(my_current_region);
  }
  assert(! my_current_region->is_in_collection_set() && my_current_region != NULL, "Never allocate from targetted or NULL region");
  assert(! my_current_region->is_humonguous(), "never attempt to allocate from humonguous object regions");

  // This isn't necessary when doing mem_allocate_locked but is for gc lab allocation.
  HeapWord* result;

  if (word_size * HeapWordSize > ShenandoahHeapRegion::RegionSizeBytes) {
    return allocate_large_memory(word_size);
  }


  do {
    result = my_current_region->par_allocate(word_size);
    if (result == NULL) {
      my_current_region = cas_update_current_region(my_current_region);
    } else {
      my_current_region->increase_live_data(word_size * HeapWordSize);
    }
  } while (result == NULL && my_current_region != NULL);

  if (result == NULL) {
    // Check if we ran out of regions and try to grow heap.
    if (my_current_region == NULL && _num_regions < _max_regions) {
      if (grow_heap_by()) {
        result = allocate_memory_work(word_size);
      }
    }
    /*
    else {
      // We reached the maximum number of regions we can allocate. Throw OOM. (We don't need this code, result
      // already is NULL.
      result = NULL;
    }
    */
  } else {
    increase_used(word_size * HeapWordSize);
    assert(! heap_region_containing(result)->is_in_collection_set(), "never allocate in targetted region");
  }
  return result;
}

HeapWord* ShenandoahHeap::allocate_large_memory(size_t words) {
  uint required_regions = (words * HeapWordSize) / ShenandoahHeapRegion::RegionSizeBytes  + 1;
  assert(required_regions <= _max_regions, "sanity check");

  HeapWord* result;
  ShenandoahHeapRegion* free_regions[required_regions];

  bool success = find_contiguous_free_regions(required_regions, free_regions);
  if (! success) {
    success = allocate_contiguous_free_regions(required_regions, free_regions);
  }
  if (! success) {
    result = NULL; // Throw OOM, we cannot allocate the huge object.
  } else {
    // Initialize huge object flags in the regions.
    free_regions[0]->set_humonguous_start(true);
    free_regions[0]->set_top(free_regions[0]->end());
    for (uint i = 1; i < required_regions; i++) {
      free_regions[i]->set_humonguous_continuation(true);
      free_regions[i]->set_top(free_regions[i]->end());
    }
    result = free_regions[0]->bottom();
  }
  return result;
}

bool ShenandoahHeap::find_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions) {
  if (ShenandoahGCVerbose) {
    tty->print_cr("trying to find %u contiguous free regions", num_free_regions);
  }
  uint free_regions_index = 0;
  for (uint regions_index = 0; regions_index < _num_regions; regions_index++) {
    // Claim a free region.
    ShenandoahHeapRegion* region = _ordered_regions[regions_index];
    bool claimed_and_free = false;
    if (region != NULL && region->claim()) {
      if (region->free() < ShenandoahHeapRegion::RegionSizeBytes) {
        region->clearClaim();
      } else {
        assert(! region->is_humonguous(), "don't reuse occupied humonguous regions");
        assert(_current_region != region, "humonguous region must not become current allocation region");
        claimed_and_free = true;
      }
    }
    if (! claimed_and_free) {
      // Not contiguous, reset search
      free_regions_index = 0;
      continue;
    }
    assert(free_regions_index >= 0 && free_regions_index < num_free_regions, "array bounds");
    free_regions[free_regions_index] = region;
    free_regions_index++;

    if (free_regions_index == num_free_regions) {
      if (ShenandoahGCVerbose) {
        tty->print_cr("found %u contiguous free regions:", num_free_regions);
        for (uint i = 0; i < num_free_regions; i++) {
          tty->print("%u: " , i);
          free_regions[i]->print();
        }
      }
      return true;
    }

  }
  if (ShenandoahGCVerbose) {
    tty->print_cr("failed to find %u free regions", num_free_regions);
  }
  return false;
}

bool ShenandoahHeap::allocate_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions) {
  // We need to be smart here to avoid interleaved allocation of regions when concurrently
  // allocating for large objects. We get the new index into regions array using CAS, where can
  // subsequently safely allocate new regions.
  int new_regions_index = ensure_new_regions(num_free_regions);
  if (new_regions_index == -1) {
    return false;
  }

  int last_new_region = new_regions_index + num_free_regions;

  // Now we can allocate new regions at the found index without being scared that
  // other threads allocate in the same contiguous region.
  if (ShenandoahGCVerbose) {
    tty->print_cr("allocate contiguous regions:");
  }
  for (int i = new_regions_index; i < last_new_region; i++) {
    ShenandoahHeapRegion* region = new ShenandoahHeapRegion();
    HeapWord* start = _first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * i;
    region->initialize(start, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize, i);
    _ordered_regions[i] = region;
    uint index = i - new_regions_index;
    assert(index >= 0 && index < num_free_regions, "array bounds");
    free_regions[index] = region;

    if (ShenandoahGCVerbose) {
      region->print();
    }
  }
  return true;
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {

  // This was used for allocation while holding the Heap_lock.
  // HeapWord* filler = allocate_memory(BrooksPointer::BROOKS_POINTER_OBJ_SIZE + size);

  HeapWord* filler = allocate_memory_gclab(BrooksPointer::BROOKS_POINTER_OBJ_SIZE + size);
  HeapWord* result = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  if (filler != NULL) {
    initialize_brooks_ptr(filler, result);
    _bytesAllocSinceCM += size * HeapWordSize;
#ifdef ASSERT
    if (ShenandoahTraceAllocations) {
      if (*gc_overhead_limit_was_exceeded)
	tty->print("gc_overhead_limit_was_exceeded");
      tty->print("mem_allocate_locked object of size %d at addr %p \n", size, result);
    }
#endif

    assert(! heap_region_containing(result)->is_in_collection_set(), "never allocate in targetted region");
    if (_concurrent_mark_in_progress) {
      mark_current_no_checks(oop(result));
    }

    return result;
  } else {
    tty->print_cr("Out of memory. Requested number of words: %x used heap: %d, bytes allocated since last CM: %d", size, used(), _bytesAllocSinceCM);
    MutexLockerEx ml(ShenandoahHeap_lock, true);
    {
      print_heap_regions();
      tty->print("Printing %d free regions:\n", _free_regions->length());
      _free_regions->print();
    }
    assert(false, "Out of memory");
    return NULL;
  }
}

class PrintOopContents: public OopClosure {
public:
  void do_oop(oop* o) {
    oop obj = *o;
    tty->print("References oop "PTR_FORMAT"\n", (HeapWord*) obj);
    obj->print();
  }

  void do_oop(narrowOop* o) {
    assert(false, "narrowOops aren't implemented");
  }
};

HeapWord*  ShenandoahHeap::mem_allocate(size_t size, 
					bool*  gc_overhead_limit_was_exceeded) {

#ifdef ASSERT
  if (ShenandoahVerify && _numAllocs > 1000000) {
    _numAllocs = 0;
  //   VM_ShenandoahVerifyHeap op(0, 0, GCCause::_allocation_failure);
  //   if (Thread::current()->is_VM_thread()) {
  //     op.doit();
  //   } else {
  //     // ...and get the VM thread to execute it.
  //     VMThread::execute(&op);
  //   }
  }
  _numAllocs++;
#endif

  // MutexLocker ml(Heap_lock);
  HeapWord* result = mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
  return result;
}

class ParallelEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* _heap;
  GCLABAllocator _allocator;

  public:
  ParallelEvacuateRegionObjectClosure(ShenandoahHeap* heap, 
				      ShenandoahAllocRegion* allocRegion) :
    _heap(heap),
    _allocator(GCLABAllocator(allocRegion)) { 
  }

  void do_object(oop p) {

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print("Calling ParallelEvacuateRegionObjectClosure on %p \n", (HeapWord*) p);
    }
#endif

    if (_heap->isMarkedCurrent(p)) {
      _heap->evacuate_object(p, &_allocator);
    }
  }

  size_t wasted() {
    return _allocator.waste();
  }
};
      

void ShenandoahHeap::initialize_brooks_ptr(HeapWord* filler, HeapWord* obj, bool new_obj) {
  BrooksPointer brooks_ptr = BrooksPointer::get(oop(obj));
  brooks_ptr.set_forwardee(oop(obj));
}

class VerifyEvacuatedObjectClosure : public ObjectClosure {

public:
  
  void do_object(oop p) {
    if (ShenandoahHeap::heap()->isMarkedCurrent(p)) {
      oop p_prime = oopDesc::bs()->resolve_oop(p);
      assert(p != p_prime, "Should point to evacuated copy");
      assert(p->klass() == p_prime->klass(), "Should have the same class");
      //      assert(p->mark() == p_prime->mark(), "Should have the same mark");
      assert(p->size() == p_prime->size(), "Should be the same size");
      assert(p_prime == oopDesc::bs()->resolve_oop(p_prime), "One forward once");
    }
  }
};

void ShenandoahHeap::verify_evacuated_region(ShenandoahHeapRegion* from_region) {
  if (ShenandoahGCVerbose) {
    tty->print("Verifying From Region\n");
    from_region->print();
  }

  VerifyEvacuatedObjectClosure verify_evacuation;
  from_region->object_iterate(&verify_evacuation);
}

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region, 
					      ShenandoahAllocRegion *alloc_region) {

  ParallelEvacuateRegionObjectClosure evacuate_region(this, alloc_region);
  
#ifdef ASSERT
  if (ShenandoahGCVerbose) {
    tty->print("parallel_evacuate_region starting from_region %d: free_regions = %d\n",  from_region->region_number(), _free_regions->available_regions());
  }
#endif

  from_region->object_iterate(&evacuate_region);
#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_evacuated_region(from_region);
  }
  if (ShenandoahGCVerbose) {
    tty->print("parallel_evacuate_region after from_region = %d: Wasted %d bytes free_regions = %d\n", from_region->region_number(), evacuate_region.wasted(), _free_regions->available_regions());
  }
#endif
}

class ParallelEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* _sh;
  ShenandoahHeapRegionSet* _cs;
  WorkGangBarrierSync* _barrier_sync;
  
public:  
  ParallelEvacuationTask(ShenandoahHeap* sh, 
			 ShenandoahHeapRegionSet* cs, 
			 WorkGangBarrierSync* barrier_sync) :
    AbstractGangTask("Parallel Evacuation Task"), 
    _cs(cs),
    _sh(sh),
    _barrier_sync(barrier_sync) {}
  
  void work(uint worker_id) {

    ShenandoahHeapRegion* from_hr = _cs->claim_next();
    ShenandoahAllocRegion allocRegion = ShenandoahAllocRegion();

    while (from_hr != NULL) {
      if (ShenandoahGCVerbose) {
     	tty->print("Thread %d claimed Heap Region %d\n",
     		   worker_id,
     		   from_hr->region_number());
	from_hr->print();
      }

      // Not sure if the check is worth it or not.
      if (from_hr->getLiveData() != 0) {
	_sh->parallel_evacuate_region(from_hr, &allocRegion);
      }

      from_hr = _cs->claim_next();
    }

    allocRegion.fill_region();

    if (ShenandoahGCVerbose) 
      tty->print("Thread %d entering barrier sync\n", worker_id);

    _barrier_sync->enter();
    if (ShenandoahGCVerbose) 
      tty->print("Thread %d post barrier sync\n", worker_id);
  }
};

class RecycleDirtyRegionsClosure: public ShenandoahHeapRegionClosure {
public:
  RecycleDirtyRegionsClosure() {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {

    if (r->is_in_collection_set()) {
      // tty->print_cr("recycling region %d:", r->region_number());
      // r->print_on(tty);
      // tty->print_cr("");
      ShenandoahHeap::heap()->decrease_used(r->used());
      r->recycle();
    }
    return false;
  }
};

void ShenandoahHeap::print_heap_regions()  {
  PrintHeapRegionsClosure pc1;
  heap_region_iterate(&pc1);
}

class PrintAllRefsOopClosure: public ExtendedOopClosure {
private:
  int _index;
  const char* _prefix;

public:
  PrintAllRefsOopClosure(const char* prefix) : _index(0), _prefix(prefix) {}

  void do_oop(oop* p)       {
    oop o = *p;
    if (o != NULL) {
      if (ShenandoahHeap::heap()->is_in(o) && o->is_oop()) {
	tty->print_cr("%s %d (%p)-> %p (marked: %d) (%s %p)", _prefix, _index, p, (HeapWord*) o, ShenandoahHeap::heap()->isMarkedCurrent(o), o->klass()->internal_name(), o->klass());
      } else {
        tty->print_cr("%s %d (%p dirty: %d) -> %p (not in heap, possibly corrupted or dirty (%d))", _prefix, _index, p, ShenandoahHeap::heap()->heap_region_containing(p)->is_in_collection_set(), (HeapWord*) o, ShenandoahHeap::heap()->heap_region_containing(o)->is_in_collection_set());
      }
    } else {
      tty->print_cr("%s %d (%p) -> %p", _prefix, _index, p, (HeapWord*) o);
    }
    _index++;
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class PrintAllRefsObjectClosure : public ObjectClosure {
  const char* _prefix;

public:
  PrintAllRefsObjectClosure(const char* prefix) : _prefix(prefix) {}

  void do_object(oop p) {
    tty->print_cr("%s object %p (marked: %d) (%s %p) refers to:", _prefix, (HeapWord*) p, ShenandoahHeap::heap()->isMarkedCurrent(p), p->klass()->internal_name(), p->klass());
    PrintAllRefsOopClosure cl(_prefix);
    p->oop_iterate(&cl);
  }
};

void ShenandoahHeap::print_all_refs(const char* prefix) {
  tty->print_cr("printing all references in the heap");
  tty->print_cr("root references:");
  PrintAllRefsOopClosure cl(prefix);
  roots_iterate(&cl);

  tty->print_cr("heap references:");
  PrintAllRefsObjectClosure cl2(prefix);
  object_iterate(&cl2);
}

class VerifyAfterMarkingOopClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;

public:
  VerifyAfterMarkingOopClosure() :
    _heap(ShenandoahHeap::heap()) { }

  void do_oop(oop* p)       {
    oop o = *p;
    if (o != NULL) {
      if (! _heap->isMarkedCurrent(o)) {
	_heap->print_heap_regions();
	_heap->print_all_refs("post-mark");
	tty->print_cr("oop not marked, although referrer is marked: %p: in_heap: %d, is_marked: %d", 
		      (HeapWord*) o, _heap->is_in(o), _heap->isMarkedCurrent(o));

        tty->print_cr("oop class: %s", o->klass()->internal_name());
	if (_heap->is_in(p)) {
	  oop referrer = oop(_heap->heap_region_containing(p)->block_start_const(p));
	  tty->print("Referrer starts at addr %p\n", (HeapWord*) referrer);
	  referrer->print();
	}
        tty->print_cr("heap region containing object:");
	_heap->heap_region_containing(o)->print();
        tty->print_cr("heap region containing referrer:");
	_heap->heap_region_containing(p)->print();
        tty->print_cr("heap region containing forwardee:");
	_heap->heap_region_containing(oopDesc::bs()->resolve_oop(o))->print();
      }
      assert(o->is_oop(), "oop must be an oop");
      assert(Metaspace::contains(o->klass()), "klass pointer must go to metaspace");
      if (! (o == oopDesc::bs()->resolve_oop(o))) {
        tty->print_cr("oops has forwardee: p: %p (%d), o = %p (%d), new-o: %p (%d)", p, _heap->heap_region_containing(p)->is_in_collection_set(), (HeapWord*) o,  _heap->heap_region_containing(o)->is_in_collection_set(), (HeapWord*) oopDesc::bs()->resolve_oop(o),  _heap->heap_region_containing(oopDesc::bs()->resolve_oop(o))->is_in_collection_set());
        tty->print_cr("oop class: %s", o->klass()->internal_name());
      }
      assert(o == oopDesc::bs()->resolve_oop(o), "oops must not be forwarded");
      assert(! _heap->heap_region_containing(o)->is_in_collection_set(), "references must not point to dirty heap regions");
      assert(_heap->isMarkedCurrent(o), "live oops must be marked current");
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class IterateMarkedCurrentObjectsClosure: public ObjectClosure {
private:
  ShenandoahHeap* _heap;
  ExtendedOopClosure* _cl;
public:
  IterateMarkedCurrentObjectsClosure(ExtendedOopClosure* cl) :
    _heap(ShenandoahHeap::heap()), _cl(cl) {};

  void do_object(oop p) {
    if (_heap->isMarkedCurrent(p)) {
      p->oop_iterate(_cl);
    }
  }

};

class IterateMarkedObjectsClosure: public ObjectClosure {
private:
  ShenandoahHeap* _heap;
  ExtendedOopClosure* _cl;
public:
  IterateMarkedObjectsClosure(ExtendedOopClosure* cl) :
    _heap(ShenandoahHeap::heap()), _cl(cl) {};

  void do_object(oop p) {
    if (_heap->isMarkedCurrent(p)) {
      p->oop_iterate(_cl);
    }
  }

};

void ShenandoahHeap::verify_heap_after_marking() {
  if (ShenandoahGCVerbose) {
    tty->print("verifying heap after marking\n");
  }
  prepare_for_verify();
  VerifyAfterMarkingOopClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);
}

void ShenandoahHeap::prepare_for_concurrent_evacuation() {

  RecycleDirtyRegionsClosure cl;
  heap_region_iterate(&cl);

  // _current_region->fill_region();

  // NOTE: This needs to be done during a stop the world pause, because
  // putting regions into the collection set concurrently with Java threads
  // will create a race. In particular, acmp could fail because when we
  // resolve the first operand, the containing region might not yet be in
  // the collection set, and thus return the original oop. When the 2nd
  // operand gets resolved, the region could be in the collection set
  // and the oop gets evacuated. If both operands have originally been
  // the same, we get false negatives.
  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions);
  regions.reclaim_humonguous_regions();
  _shenandoah_policy->choose_collection_and_free_sets(&regions, _collection_set, _free_regions);

}

class ShenandoahUpdateRootsClosure: public ExtendedOopClosure {

  void do_oop(oop* p)       {
    ShenandoahHeap::heap()->maybe_update_oop_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahHeap::update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  ShenandoahUpdateRootsClosure cl;
  roots_iterate(&cl);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

class ShenandoahUpdateObjectsClosure : public ObjectClosure {
  ShenandoahUpdateRootsClosure _refs_cl;
  ShenandoahHeap* _heap;

public:
  ShenandoahUpdateObjectsClosure() :
    _refs_cl(ShenandoahUpdateRootsClosure()),
    _heap(ShenandoahHeap::heap()) {
  }

  void do_object(oop p) {
    assert(ShenandoahHeap::heap()->is_in(p), "only update objects in heap (where else?)");
    if (_heap->isMarkedCurrent(p)) {
      p->oop_iterate(&_refs_cl);
    }
  }
};

class ParallelUpdateRefsTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;
  ShenandoahUpdateObjectsClosure _update_refs_cl;

public:
  ParallelUpdateRefsTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Update References Task"), 
  _regions(regions),
    _update_refs_cl(ShenandoahUpdateObjectsClosure()) {
  }

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions->claim_next();

    while (region != NULL) {
      if (! region->is_in_collection_set()) {
        region->object_iterate(&_update_refs_cl);
      }
      region = _regions->claim_next();
    }
  }
};

void ShenandoahHeap::update_references() {

  update_roots();
  ShenandoahUpdateObjectsClosure cl;

  for (uint i = 0; i < _num_regions; i++) {
    _ordered_regions[i]->clearClaim();
  }

  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions);
  ParallelUpdateRefsTask task = ParallelUpdateRefsTask(&regions);
  workers()->run_task(&task);

  for (uint i = 0; i < _num_regions; i++) {
    _ordered_regions[i]->clearClaim();
  }
  _current_region->claim();
}


class ShenandoahEvacuateUpdateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  HeapAllocator _allocator;
public:
  ShenandoahEvacuateUpdateRootsClosure() :
    _heap(ShenandoahHeap::heap()),
    _allocator(HeapAllocator()) {
  }

  void do_oop(oop* p) {
    assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

    oop obj = *p;
    if (obj != NULL && _heap->heap_region_containing(obj)->is_in_collection_set()) {
      *p = _heap->evacuate_object(*p, &_allocator);
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};


void ShenandoahHeap::evacuate_and_update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  ShenandoahEvacuateUpdateRootsClosure cl;
  roots_iterate(&cl);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}


void ShenandoahHeap::do_evacuation() {
  assert(Thread::current()->is_VM_thread() || ShenandoahConcurrentEvacuation, "Only evacuate from VMThread unless we do concurrent evacuation");

  parallel_evacuate();

  if (! ShenandoahConcurrentEvacuation) {
    // We need to make sure that after leaving the safepoint, all
    // GC roots are up-to-date. This is an assumption built into
    // the hotspot compilers, especially C2, that allows it to
    // do optimizations like lifting barriers outside of a loop.

    update_roots();
  }

  set_evacuation_in_progress(false);

  if (! ShenandoahConcurrentUpdateRefs) {
    update_references();
  }

  reset_mark_bitmap();

  if (ShenandoahVerify) {
    VM_ShenandoahVerifyHeapAfterEvacuation verify_after_evacuation;
    if (Thread::current()->is_VM_thread()) {
      verify_after_evacuation.doit();
    } else {
      VMThread::execute(&verify_after_evacuation);
    }
  }

}

void ShenandoahHeap::parallel_evacuate() {

  assert(Thread::current()->is_VM_thread() || ShenandoahConcurrentEvacuation, "Only evacuate from VMThread unless we do concurrent evacuation");

  if (ShenandoahGCVerbose) {
    tty->print_cr("starting parallel_evacuate");
    //    PrintHeapRegionsClosure pc1;
    //    heap_region_iterate(&pc1);
  }

  _shenandoah_policy->record_concurrent_evacuation_start();

  if (ShenandoahGCVerbose) {
    tty->print("Printing all available regions");
    print_heap_regions();
  }

  if (ShenandoahPrintCollectionSet) {
    tty->print("Printing collection set which contains %d regions:\n", _collection_set->available_regions());
    _collection_set->print();
  }

  barrierSync.set_n_workers(_max_workers);
  
  ParallelEvacuationTask evacuationTask = ParallelEvacuationTask(this, _collection_set, &barrierSync);

  workers()->run_task(&evacuationTask);

  if (ShenandoahGCVerbose) {

    tty->print("Printing postgc collection set which contains %d regions:\n", _collection_set->available_regions());
    _collection_set->print();

    tty->print("Printing postgc free regions which contain %d free regions:\n", _free_regions->available_regions());
    _free_regions->print();

    tty->print_cr("finished parallel_evacuate");
    PrintHeapRegionsClosure pc2;
    heap_region_iterate(&pc2);

    tty->print_cr("all regions after evacuation:");
    print_heap_regions();
  }

  _shenandoah_policy->record_concurrent_evacuation_end();
}

class VerifyEvacuationClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
  ShenandoahHeapRegion* _from_region;

public:
  VerifyEvacuationClosure(ShenandoahHeapRegion* from_region) :
    _heap(ShenandoahHeap::heap()), _from_region(from_region) { }

  void do_oop(oop* p)       {
    oop heap_oop = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(heap_oop)) {
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: %p", (HeapWord*) heap_oop));
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahHeap::roots_iterate(ExtendedOopClosure* cl) {

  assert(Thread::current()->is_VM_thread(), "Only iterate roots while world is stopped");

  CodeBlobToOopClosure blobsCl(cl, false);
  KlassToOopClosure klassCl(cl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  ClassLoaderDataGraph::clear_claimed_marks();

  process_strong_roots(true, false, ScanningOption(so), cl, &blobsCl, &klassCl);
  process_weak_roots(cl, &blobsCl);
}

void ShenandoahHeap::verify_evacuation(ShenandoahHeapRegion* from_region) {

  VerifyEvacuationClosure rootsCl(from_region);
  roots_iterate(&rootsCl);

}

oop ShenandoahHeap::maybe_update_oop_ref(oop* p) {

  assert((! is_in(p)) || (! heap_region_containing(p)->is_in_collection_set()), "never update refs in from-space"); 

  oop heap_oop = *p; // read p
  if (! oopDesc::is_null(heap_oop)) {

#ifdef ASSERT
    if (! is_in(heap_oop)) {
      print_heap_regions();
      tty->print_cr("object not in heap: %p, referenced by: %p", (HeapWord*) heap_oop, p);
      assert(is_in(heap_oop), "object must be in heap");
    }
#endif
    assert(is_in(heap_oop), "only ever call this on objects in the heap");
    assert(! (is_in(p) && heap_region_containing(p)->is_in_collection_set()), "we don't want to update references in from-space");
    oop forwarded_oop = ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop); // read brooks ptr
    if (forwarded_oop != heap_oop) {
      // tty->print_cr("updating old ref: %p pointing to %p to new ref: %p", p, heap_oop, forwarded_oop);
      assert(forwarded_oop->is_oop(), "oop required");
      // If this fails, another thread wrote to p before us, it will be logged in SATB and the
      // reference be updated later.
      oop result = (oop) Atomic::cmpxchg_ptr(forwarded_oop, p, heap_oop);

      if (result == heap_oop) { // CAS successful.
	  return forwarded_oop;
      }
    } else {
      return forwarded_oop;
    }
    /*
      else {
      tty->print_cr("not updating ref: %p", heap_oop);
      }
    */
  }
  return NULL;
}

bool ShenandoahHeap::supports_tlab_allocation() const {
  return true;
}


size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  ShenandoahHeapRegion* my_current_region = _current_region;
  // This can happen during initialization.
  if (my_current_region == NULL)
    return MinTLABSize;
  return MIN2(my_current_region->free(), (size_t) MinTLABSize);
}

bool  ShenandoahHeap::can_elide_tlab_store_barriers() const {
  return true;
}

bool  ShenandoahHeap::can_elide_initializing_store_barrier(oop new_obj) {
  return true;
}

bool ShenandoahHeap::card_mark_must_follow_store() const {
  return false;
}

bool ShenandoahHeap::supports_heap_inspection() const {
  return false;
}

size_t ShenandoahHeap::unsafe_max_alloc() {
  return ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
}

void ShenandoahHeap::collect(GCCause::Cause cause) {
  if (cause == GCCause::_gc_locker) {
    assert(is_waiting_for_jni_before_gc(), "must be waiting for JNI, why enter this otherwise?");

    // This kicks off concurrent marking in the GC background thread.
    set_evacuation_in_progress(true);

    // We need to do non-concurrent marking right now, before we release the flag below.
    // The GC background thread is waiting on it and would start another marking
    // cycle otherwise.
    if (! ShenandoahConcurrentEvacuation) {
      VM_ShenandoahEvacuation evacuation;
      VMThread::execute(&evacuation);
    }

    // The GC background thread is waiting on this flag. Get it going again.
    set_waiting_for_jni_before_gc(false);
  }
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  Unimplemented();
  return NULL;
  
}

ShenandoahCollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _shenandoah_policy;
}


HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  Space* sp = space_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_is_obj(addr);
}

jlong ShenandoahHeap::millis_since_last_gc() {
  return 0;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  st->print_cr("Not yet implemented: ShenandoahHeap::print_gc_threads_on()");
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  // Ignore this for now.  No gc threads.
}

void ShenandoahHeap::print_tracing_info() const {
  if (PrintGCDetails) {
    _shenandoah_policy->print_tracing_info();
  }
}

class ShenandoahVerifyRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
  VerifyOption     _vo;
  bool             _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  ShenandoahVerifyRootsClosure(VerifyOption vo) :
    _heap(ShenandoahHeap::heap()),
    _vo(vo),
    _failures(false) { }

  bool failures() { return _failures; }

  void do_oop(oop* p)       {
    if (*p != NULL) {
      oop heap_oop = oopDesc::load_heap_oop(p);
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      if (!obj->is_oop()) {
        { // Just for debugging.
	  gclog_or_tty->print_cr("Root location "PTR_FORMAT" "
				 "verified "PTR_FORMAT, p, (void*) obj);
	  obj->print_on(gclog_or_tty);
        }
      }
      guarantee(obj->is_oop(), "is_oop");
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class ShenandoahVerifyHeapClosure: public ObjectClosure {
private:
  ShenandoahVerifyRootsClosure _rootsCl;
public:
  ShenandoahVerifyHeapClosure(ShenandoahVerifyRootsClosure rc) :
    _rootsCl(rc) {};

  void do_object(oop p) {
    _rootsCl.do_oop(&p);
  }
};

class ShenandoahVerifyKlassClosure: public KlassClosure {
  OopClosure *_oop_closure;
 public:
  ShenandoahVerifyKlassClosure(OopClosure* cl) : _oop_closure(cl) {}
  void do_klass(Klass* k) {
    k->oops_do(_oop_closure);
  }
};

void ShenandoahHeap::verify(bool silent , VerifyOption vo) {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {

    ShenandoahVerifyRootsClosure rootsCl(vo);

    assert(Thread::current()->is_VM_thread(),
	   "Expected to be executed serially by the VM thread at this point");

    roots_iterate(&rootsCl);

    bool failures = rootsCl.failures();
    if (ShenandoahGCVerbose)
      gclog_or_tty->print("verify failures: %d", failures); 

    ShenandoahVerifyHeapClosure heapCl(rootsCl);

    object_iterate(&heapCl);
    // TODO: Implement rest of it.
#ifdef ASSERT_DISABLED
    verify_live();
#endif
  } else {
    if (!silent) gclog_or_tty->print("(SKIPPING roots, heapRegions, remset) ");
  }
}
size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return ShenandoahHeapRegion::RegionSizeBytes;
}

class ShenandoahIterateObjectClosureRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosure* _cl;
public:
  ShenandoahIterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate(_cl);
    return false;
  }
};

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  ShenandoahIterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  Unimplemented();
}

class ShenandoahIterateOopClosureRegionClosure : public ShenandoahHeapRegionClosure {
  MemRegion _mr;
  ExtendedOopClosure* _cl;
  bool _skip_unreachable_objects;
public:
  ShenandoahIterateOopClosureRegionClosure(ExtendedOopClosure* cl, bool skip_unreachable_objects) :
    _cl(cl), _skip_unreachable_objects(skip_unreachable_objects) {}
  ShenandoahIterateOopClosureRegionClosure(MemRegion mr, ExtendedOopClosure* cl) 
    :_mr(mr), _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->oop_iterate(_cl, _skip_unreachable_objects);
    return false;
  }
};

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl, bool skip_dirty_regions, bool skip_unreachable_objects) {
  ShenandoahIterateOopClosureRegionClosure blk(cl, skip_unreachable_objects);
  heap_region_iterate(&blk, skip_dirty_regions);
}

void ShenandoahHeap::oop_iterate(MemRegion mr, 
				 ExtendedOopClosure* cl) {
  ShenandoahIterateOopClosureRegionClosure blk(mr, cl);
  heap_region_iterate(&blk);
}

void  ShenandoahHeap::object_iterate_since_last_GC(ObjectClosure* cl) {
  Unimplemented();
}

class SpaceClosureRegionClosure: public ShenandoahHeapRegionClosure {
  SpaceClosure* _cl;
public:
  SpaceClosureRegionClosure(SpaceClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    _cl->do_space(r);
    return false;
  }
};

void  ShenandoahHeap::space_iterate(SpaceClosure* cl) {
  SpaceClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

ShenandoahHeapRegion*
ShenandoahHeap::heap_region_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr) & ~(ShenandoahHeapRegion::RegionSizeBytes - 1);
  uintptr_t index = (region_start - (uintptr_t) _first_region_bottom) >> ShenandoahHeapRegion::RegionSizeShift;
  ShenandoahHeapRegion* result = _ordered_regions[index];
  assert(addr >= result->bottom() && addr < result->end(), "address must be in found region");
  return result;
}

Space*  ShenandoahHeap::space_containing(const void* oop) const {
  Space* res = heap_region_containing(oop);
  return res;
}

void  ShenandoahHeap::gc_prologue(bool b) {
  Unimplemented();
}

void  ShenandoahHeap::gc_epilogue(bool b) {
  Unimplemented();
}

// Apply blk->doHeapRegion() on all committed regions in address order,
// terminating the iteration early if doHeapRegion() returns true.
void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions) const {
  for (size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* current  = _ordered_regions[i];
    if (current->is_humonguous_continuation()) {
      continue;
    }
    if ( !(skip_dirty_regions && current->is_in_collection_set()) && blk->doHeapRegion(current)) 
      return;
  }
}

/**
 * Maybe we need that at some point...
oop* ShenandoahHeap::resolve_oop_ptr(oop* p) {
  if (is_in(p) && heap_region_containing(p)->is_dirty()) {
    // If the reference is in an object in from-space, we need to first
    // find its to-space counterpart.
    // TODO: This here is slow (linear search inside region). Make it faster.
    oop from_space_oop = oop_containing_oop_ptr(p);
    HeapWord* to_space_obj = (HeapWord*) oopDesc::bs()->resolve_oop(from_space_oop);
    return (oop*) (to_space_obj + ((HeapWord*) p - ((HeapWord*) from_space_oop)));
  } else {
    return p;
  }
}

oop ShenandoahHeap::oop_containing_oop_ptr(oop* p) {
  HeapWord* from_space_ref = (HeapWord*) p;
  ShenandoahHeapRegion* region = heap_region_containing(from_space_ref);
  HeapWord* from_space_obj = NULL;
  for (HeapWord* curr = region->bottom(); curr < from_space_ref; ) {
    oop curr_obj = (oop) curr;
    if (curr < from_space_ref && from_space_ref < (curr + curr_obj->size())) {
      from_space_obj = curr;
      break;
    } else {
      curr += curr_obj->size();
    }
  }
  assert (from_space_obj != NULL, "must not happen");
  oop from_space_oop = (oop) from_space_obj;
  assert (from_space_oop->is_oop(), "must be oop");
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop(((HeapWord*) from_space_oop) - BrooksPointer::BROOKS_POINTER_OBJ_SIZE)), "oop must have a brooks ptr");
  return from_space_oop;
}
 */

ShenandoahMarkRefsClosure::ShenandoahMarkRefsClosure(uint worker_id) :
  _worker_id(worker_id),
  _heap(ShenandoahHeap::heap()),
  _mark_objs(ShenandoahMarkObjsClosure(worker_id))
{
}

void ShenandoahMarkRefsClosure::do_oop_work(oop* p) {
  // We piggy-back reference updating to the marking tasks.
  // tty->print_cr("marking oop ref: %p", p);
  oop* old = p;
  oop obj = _heap->maybe_update_oop_ref(p);
#ifdef ASSERT
  if (ShenandoahTraceUpdates) {
    if (p != old) 
      tty->print("Update %p => %p  to %p => %p\n", p, (HeapWord*) *p, old, (HeapWord*) *old);
  }
#endif

  // NOTE: We used to assert the following here. This does not always work because
  // a concurrent Java thread could change the the field after we updated it.
  // oop obj = oopDesc::load_heap_oop(p);
  // assert(oopDesc::bs()->resolve_oop(obj) == *p, "we just updated the referrer");
  // assert(obj == NULL || ! _heap->heap_region_containing(obj)->is_dirty(), "must not point to dirty region");

  _mark_objs.do_object(obj);
}

void ShenandoahMarkRefsClosure::do_oop(narrowOop* p) {
  assert(false, "narrowOops not supported");
}

void ShenandoahMarkRefsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

ShenandoahMarkRefsNoUpdateClosure::ShenandoahMarkRefsNoUpdateClosure(uint worker_id) :
  _worker_id(worker_id),
  _heap(ShenandoahHeap::heap()),
  _mark_objs(ShenandoahMarkObjsClosure(worker_id))
{
}

void ShenandoahMarkRefsNoUpdateClosure::do_oop(narrowOop* p) {
  assert(false, "narrowOops not supported");
}

void ShenandoahMarkRefsNoUpdateClosure::do_oop(oop* p) {
  oop obj = *p;
  if (! oopDesc::is_null(obj)) {
    _mark_objs.do_object(obj);
  }
}

ShenandoahMarkObjsClosure::ShenandoahMarkObjsClosure(uint worker_id) :
  _worker_id(worker_id),
  _heap(ShenandoahHeap::heap()),
  _concurrent_mark(_heap->concurrentMark()) {
}

void ShenandoahMarkObjsClosure::do_object(oop obj) {
  if (obj != NULL) {

#ifdef ASSERT
    if (_heap->heap_region_containing(obj)->is_in_collection_set()) {
      tty->print_cr("trying to mark obj: %p (%d) in dirty region: ", (HeapWord*) obj, _heap->isMarkedCurrent(obj));
      _heap->heap_region_containing(obj)->print();
      _heap->print_heap_regions();
    }
#endif
    assert(! _heap->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");
    assert(_heap->is_in(obj), "referenced objects must be in the heap. No?");
    if (_heap->mark_current(obj)) {
#ifdef ASSERT
      if (ShenandoahTraceConcurrentMarking)
	tty->print_cr("marked obj: %p", (HeapWord*) obj);
#endif
      // Calculate liveness of heap region containing object.
      ShenandoahHeapRegion* region = _heap->heap_region_containing(obj);
      region->increase_live_data((obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE) * HeapWordSize);
      _concurrent_mark->addTask(obj, _worker_id);
    }
#ifdef ASSERT
    else {
      if (ShenandoahTraceConcurrentMarking) {
        tty->print_cr("failed to mark obj (already marked): %p", (HeapWord*) obj);
      }
      assert(_heap->isMarkedCurrent(obj), "make sure object is marked");
    }
#endif

    /*
    else {
      tty->print_cr("already marked object %p, %d", obj, getMark(obj)->age());
    }
    */
  }
  /*
  else {
    if (obj != NULL) {
      tty->print_cr("not marking root object because it's not in heap: %p", obj);
    }
  }
  */
}

void ShenandoahHeap::prepare_unmarked_root_objs() {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  OopsInGenClosure* cl;
  ShenandoahMarkRefsClosure rootsCl1(0);
  ShenandoahMarkRefsNoUpdateClosure rootsCl2(0);
  if (ShenandoahConcurrentUpdateRefs) {
    cl = &rootsCl1;
  } else {
    cl = &rootsCl2;
  }
  roots_iterate(cl);
}

class ClearLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  ClearLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->clearLiveData();
    r->clearClaim();
    return false;
  }
};


void ShenandoahHeap::start_concurrent_marking() {
  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  if (UseTLAB) {
    for (JavaThread* t = Threads::first(); t; t = t->next()) {
      t->tlab().make_parsable(true);
    }
  }

  _shenandoah_policy->record_bytes_allocated(_bytesAllocSinceCM);
  _bytesAllocSinceCM = 0;

#ifdef ASSERT
  if (ShenandoahDumpHeapBeforeConcurrentMark) {
    prepare_for_verify();
    print_all_refs("pre-mark");
  }
#endif
  
  ClearLivenessClosure clc(this);
  heap_region_iterate(&clc);

  // We need to claim the current region here, because we just cleared the claimed
  // marks in all regions, and the current region might otherwise get used for a humonguous
  // region.
  _current_region->claim();

  // print_all_refs("pre -mark");

  // oopDesc::_debug = true;

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  prepare_unmarked_root_objs();

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

  //  print_all_refs("pre-mark2");
}


class VerifyLivenessClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyLivenessClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(_sh->heap_region_containing(obj)->is_in_collection_set() == (obj != oopDesc::bs()->resolve_oop(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %d, is_forwarded: %d",
                        _sh->heap_region_containing(obj)->is_in_collection_set(),
                        obj != oopDesc::bs()->resolve_oop(obj))
                );
      obj = oopDesc::bs()->resolve_oop(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_in_collection_set(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (! sh->isMarkedCurrent(obj)) {
        sh->print_on(tty);
      }
      assert(sh->isMarkedCurrent(obj), err_msg("Referenced Objects should be marked obj: %p, marked: %d, is_in_heap: %d", 
                                               (HeapWord*) obj, sh->isMarkedCurrent(obj), sh->is_in(obj)));
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_live() {

  VerifyLivenessClosure cl;
  roots_iterate(&cl);

  IterateMarkedObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);

}

class VerifyAfterEvacuationClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyAfterEvacuationClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(_sh->heap_region_containing(obj)->is_in_collection_set() == (obj != oopDesc::bs()->resolve_oop(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %d, is_forwarded: %d",
                        _sh->heap_region_containing(obj)->is_in_collection_set(),
                        obj != oopDesc::bs()->resolve_oop(obj))
                );
      obj = oopDesc::bs()->resolve_oop(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_in_collection_set(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      guarantee(Metaspace::contains(obj->klass()), "klass pointer must go to metaspace");
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_heap_after_evacuation() {

  prepare_for_verify();

  VerifyAfterEvacuationClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);

}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(concurrent_mark_in_progress(), "How else could we get here?");
  set_concurrent_mark_in_progress(false);

  if (ShenandoahGCVerbose) {
    PrintHeapRegionsClosure pc;
    heap_region_iterate(&pc);
  }

#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_heap_after_marking();
  }

#endif
}

bool ShenandoahHeap::should_start_concurrent_marking() {
  return ! Atomic::cmpxchg(true, &_concurrent_mark_in_progress, false);
}

bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  _concurrent_mark_in_progress = in_progress;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, ! in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  _evacuation_in_progress = in_progress;
  OrderAccess::storeload();
}

bool ShenandoahHeap::is_evacuation_in_progress() {
  return _evacuation_in_progress;
}

void ShenandoahHeap::set_waiting_for_jni_before_gc(bool wait_for_jni) {
  _waiting_for_jni_before_gc = wait_for_jni;
  OrderAccess::storeload();
}

bool ShenandoahHeap::is_waiting_for_jni_before_gc() {
  return _waiting_for_jni_before_gc;
}

void ShenandoahHeap::post_allocation_collector_specific_setup(HeapWord* hw) {
  oop obj = oop(hw);

  // Assuming for now that objects can't be created already locked
  assert(! obj->has_displaced_mark(), "hopefully new objects don't have displaced mark");
  // tty->print_cr("post_allocation_collector_specific_setup:: %p", obj);

  if (_concurrent_mark_in_progress) {
    mark_current_no_checks(obj);
  }
}

/*
 * Marks the object. Returns true if the object has not been marked before and has
 * been marked by this thread. Returns false if the object has already been marked,
 * or if a competing thread succeeded in marking this object.
 */
bool ShenandoahHeap::mark_current(oop obj) const {
  assert(obj == oopDesc::bs()->resolve_oop(obj), "only mark forwarded copy of objects");
  return mark_current_no_checks(obj);
}

bool ShenandoahHeap::mark_current_no_checks(oop obj) const {
  return _next_mark_bit_map->parMark((HeapWord*) obj);
}

bool ShenandoahHeap::isMarkedCurrent(oop obj) const {
  return _next_mark_bit_map->isMarked((HeapWord*) obj);
}

void ShenandoahHeap::verify_copy(oop p,oop c){
    assert(p != oopDesc::bs()->resolve_oop(p), "forwarded correctly");
    assert(oopDesc::bs()->resolve_oop(p) == c, "verify pointer is correct");
    if (p->klass() != c->klass()) {
      print_heap_regions();
    }
    assert(p->klass() == c->klass(), err_msg("verify class p-size: %d c-size: %d", p->size(), c->size()));
    assert(p->size() == c->size(), "verify size");
    // Object may have been locked between copy and verification
    //    assert(p->mark() == c->mark(), "verify mark");
    assert(c == oopDesc::bs()->resolve_oop(c), "verify only forwarded once");
  }

void ShenandoahHeap::copy_object(oop p, HeapWord* s) {
  HeapWord* filler = s;
  assert(s != NULL, "allocation of brooks pointer must not fail");
  HeapWord* copy = s + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  assert(copy != NULL, "allocation of copy object must not fail");
  Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
  initialize_brooks_ptr(filler, copy);

#ifdef ASSERT
  if (ShenandoahTraceEvacuations) {
    tty->print_cr("copy object from %p to: %p", (HeapWord*) p, copy);
  }
#endif
}

oop ShenandoahHeap::evacuate_object(oop p, EvacuationAllocator* allocator) {

  size_t required = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
  HeapWord* filler = allocator->allocate(required);
  HeapWord* copy = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  copy_object(p, filler);
  HeapWord* result = BrooksPointer::get(p).cas_forwardee((HeapWord*) p, copy);

  oop return_val;
  if (result == (HeapWord*) p) {
    return_val = oop(copy);

    if (! ShenandoahConcurrentUpdateRefs) {
      mark_current(return_val);
    }

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print("Copy of %p to %p succeeded \n", (HeapWord*) p, copy);
    }
#endif
  }  else {
    allocator->rollback(filler, required);
#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print("Copy of %p to %p \n", (HeapWord*) p, copy);
    }
#endif
    return_val = (oopDesc*) result;
  }

  return return_val;
}

HeapWord* ShenandoahHeap::allocate_from_tlab_work(Thread* thread, size_t size) {
  return CollectedHeap::allocate_from_tlab_work(SystemDictionary::Object_klass(), thread, size);
}

HeapWord* ShenandoahHeap::tlab_post_allocation_setup(HeapWord* obj, bool new_obj) {
  HeapWord* result = obj + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  initialize_brooks_ptr(obj, result, new_obj);
  return result;
}

uint ShenandoahHeap::oop_extra_words() {
  return BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
}

bool ShenandoahHeap::grow_heap_by() {
  int new_region_index = ensure_new_regions(1);
  if (new_region_index != -1) {
    ShenandoahHeapRegion* new_region = new ShenandoahHeapRegion();
    HeapWord* start = _first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * new_region_index;
    new_region->initialize(start, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize, new_region_index);
    if (ShenandoahGCVerbose) {
      tty->print_cr("allocating new region at index: %d", new_region_index);
      new_region->print();
    }
    _ordered_regions[new_region_index] = new_region;
    _free_regions->append(new_region);
    return true;
  } else {
    return false;
  }
}

int ShenandoahHeap::ensure_new_regions(int new_regions) {
  /*
  while (true) {

    jlong num_regions = _num_regions;
    jlong new_num_regions = num_regions + new_regions;
    if (new_num_regions >= _max_regions) {
      // Not enough regions left.
      return -1;
    }

    jlong old = Atomic::cmpxchg(new_num_regions, &_num_regions, num_regions);
    if (old == num_regions) {
      // CAS Successful. Expand virtual memory and return the index.
      size_t expand_size = new_regions * ShenandoahHeapRegion::RegionSizeBytes;
      // if (ShenandoahGCVerbose) {
        tty->print_cr("expanding storage by %x bytes, for %d new regions", expand_size, new_regions);
        // }
      bool success = _storage.expand_by(expand_size);
      assert(success, "should always be able to expand by requested size");

      return num_regions;
    }
  }
  */

  //  MutexLockerEx ml(ShenandoahHeap_lock, true);
  // {
    size_t num_regions = _num_regions;
    size_t new_num_regions = num_regions + new_regions;
    if (new_num_regions >= _max_regions) {
      // Not enough regions left.
      return -1;
    }

    size_t expand_size = new_regions * ShenandoahHeapRegion::RegionSizeBytes;
    if (ShenandoahGCVerbose) {
      tty->print_cr("expanding storage by %x bytes, for %d new regions", expand_size, new_regions);
    }
    bool success = _storage.expand_by(expand_size);
    assert(success, "should always be able to expand by requested size");

    _num_regions = new_num_regions;

    // We need a memory barrier here to prevent subsequent threads from loading
    // a cached value of _num_regions.
    OrderAccess::storeload();

    return num_regions;
    // }

}

#ifndef CC_INTERP
void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
  __ incrementq(obj, BrooksPointer::BROOKS_POINTER_OBJ_SIZE * HeapWordSize);
  __ movptr(Address(obj, -1 * HeapWordSize), obj);
}
#endif

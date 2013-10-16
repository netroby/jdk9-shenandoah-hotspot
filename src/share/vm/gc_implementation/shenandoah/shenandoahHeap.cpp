#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "utilities/copy.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "runtime/atomic.inline.hpp"

ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

void printHeapLocations(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  int *val = NULL;
  for (cur = start; cur < end; cur++) {
      val = (int *) cur;
      tty->print(PTR_FORMAT":"PTR_FORMAT"\n", val, *val);
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
    _st->print("Region %d bottom = "PTR_FORMAT" end = "PTR_FORMAT" top = "PTR_FORMAT" used = %x free = %x live = %x dirty: %d\n", 
	       r->regionNumber, r->bottom(), r->end(), r->top(), r->used(), r->free(), r->getLiveData(), r->is_dirty());
    return false;
  }
};

class PrintHeapObjectsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x\n", 
	       r->regionNumber, r->top(), r->used(), r->free());
    
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
						 ShenandoahHeapRegion::RegionSizeBytes);
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*) (heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet());
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);
  _storage.initialize(pgc_rs, max_byte_size);
  if (ShenandoahGCVerbose) {
    tty->print("Calling initialize on reserved space base = %p end = %p\n", 
	       pgc_rs.base(), pgc_rs.base() + pgc_rs.size());
  }

  _numRegions = init_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  _ordered_regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _numRegions, mtGC); 

  ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
  _first_region = current;
  _initialSize = _numRegions * ShenandoahHeapRegion::RegionSizeBytes;
  size_t regionSizeWords = ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
  assert(init_byte_size == _initialSize, "tautology");
  _regions = new ShenandoahHeapRegionSet(_numRegions);
  _free_regions = new ShenandoahHeapRegionSet(_numRegions);
  _collection_set = new ShenandoahHeapRegionSet(_numRegions);

  _ordered_regions[0] = current;

  for (size_t i = 0; i < _numRegions - 1; i++) {
    ShenandoahHeapRegion* next = new ShenandoahHeapRegion();
    current->initialize((HeapWord*) pgc_rs.base() + 
			regionSizeWords * i, regionSizeWords);
    current->regionNumber = i;
    _regions->put(i, current);
    _free_regions->put(i, current);
    current = next;
    _ordered_regions[i+1] = current;
  }
  _last_region = current;

  size_t last_region = _numRegions - 1;
  current->initialize((HeapWord*) pgc_rs.base() + regionSizeWords * (last_region), 
		      regionSizeWords);

  current->regionNumber = last_region;
  _regions->put(last_region, current);
  _free_regions->put(last_region, current);
  _numAllocs = 0;
  if (ShenandoahGCVerbose) {
    tty->print("All Regions\n");
    _regions->print();
    tty->print("Free Regions\n");
    _free_regions->print();
  }

  _current_region = _free_regions->get_next();

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);


  JavaThread::update_refs_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  _concurrent_gc_thread = new ShenandoahConcurrentGCThread();
  _concurrent_gc_thread->start();
  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) : 
  SharedHeap(policy),
  _pgc_policy(policy), 
  _concurrent_mark_in_progress(false),
  _epoch(1),
  _regions(NULL),
  _free_regions(NULL),
  _collection_set(NULL),
  _bytesAllocSinceCM(0),
  _max_workers((int) MAX2((uint)ParallelGCThreads, 1U)),
  _default_gclab_size(1024){
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
}


void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("Shenandoah Heap");
  st->print(" total = " SIZE_FORMAT " K, used " SIZE_FORMAT " K ", capacity()/ K, used() /K);
  st->print("Region size = " SIZE_FORMAT "K, ", ShenandoahHeapRegion::RegionSizeBytes / K);
  PrintHeapRegionsClosure cl(st);
  heap_region_iterate(&cl);
}

void ShenandoahHeap::post_initialize() {
  //  ShenandoahConcurrentThread* first = new ShenandoahConcurrentThread();
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
  CalculateUsedRegionClosure calc;
  heap_region_iterate(&calc);
  return calc.getResult();
}


size_t ShenandoahHeap::capacity() const {
  return _numRegions * ShenandoahHeapRegion::RegionSizeBytes;

}

bool ShenandoahHeap::is_maximal_no_gc() const {
  Unimplemented();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return _numRegions * ShenandoahHeapRegion::RegionSizeBytes;
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
  return p > _first_region->bottom() && p < _last_region->end();
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
  Unimplemented();
  bool* limit_exceeded;
  return mem_allocate(word_size, limit_exceeded);
}


HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
  HeapWord* result = allocate_memory_gclab(word_size);
  assert(! heap_region_containing(result)->is_dirty(), "Never allocate in dirty region");
  if (result != NULL) {
    _current_region->increase_live_data(((jlong)word_size) * HeapWordSize);
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
    if ((! r->is_dirty()) && r->free() >= _required_size) {
      _result = r;
      return true;
    }
    return false;
  }
  ShenandoahHeapRegion* result() { return _result;}

};

void ShenandoahHeap::update_current_region() {
  if (ShenandoahGCVerbose) {
     tty->print("Old current region = ");
     _current_region->print();
  }

  do {
    if (_free_regions->has_next()) {
      _current_region = _free_regions->get_next();
    } else {
      if (ShenandoahGCVerbose) {
        print_heap_regions();
        tty->print_cr("free regions:");
        _free_regions->print();
      }
      assert(false, "No GC implemented");
    }
  } while (_current_region->is_dirty());

  if (ShenandoahGCVerbose) {
    tty->print("New current region = ");
    _current_region->print();
  }
}

ShenandoahHeapRegion* ShenandoahHeap::cas_update_current_region(ShenandoahHeapRegion* expected) {

  
  if (_free_regions->has_next()) {
    ShenandoahHeapRegion* previous = (ShenandoahHeapRegion*) Atomic::cmpxchg_ptr(_free_regions->peek_next(), &_current_region, expected);
    if (previous == expected) {
      // Advance the region set.
      _free_regions->get_next();
    }
    // If the above CAS fails, we want the caller to get the _current_region that the other thread
    // CAS'ed.
    return _current_region;
  } else {
    assert(false, "No GC implemented");
  }

}

HeapWord* ShenandoahHeap::allocate_memory_gclab(size_t size) {
  ShenandoahHeapRegion* my_current_region = _current_region;
  assert(! my_current_region->is_dirty(), "never get dirty regions in free-lists");
  if (my_current_region == NULL) {
    my_current_region = cas_update_current_region(my_current_region);
  }
  assert(! my_current_region->is_dirty() && my_current_region != NULL, "Never allocate from dirty or NULL region");

  // This isn't necessary when doing mem_allocate_locked but is for gc lab allocation.
  HeapWord* result;
  do {
    result = my_current_region->par_allocate(size);
    if (result == NULL) {
      my_current_region = cas_update_current_region(my_current_region);
    }
  } while (result == NULL && my_current_region != NULL);

  if (result == NULL) {
    assert(my_current_region == NULL, "This can only happen if we run out of regions");
    assert(false, "Failed to allocate object");
  } else {
    assert(! heap_region_containing(result)->is_dirty(), "never allocate in dirty region");
  }
  return result;
}

HeapWord* ShenandoahHeap::allocate_memory(size_t size) {
  if (_current_region->is_dirty() || (_current_region == NULL))
    update_current_region();

  assert(! _current_region->is_dirty() && _current_region != NULL, "Never allocate from dirty or NULL region");

  HeapWord* result = _current_region->allocate(size);
  // Try next free region.
  if (result == NULL) {
    update_current_region();
    result = _current_region->par_allocate(size);
  }
  if (result != NULL) {
    assert(! heap_region_containing(result)->is_dirty(), "never allocate in dirty region");
    return result;
  } else {
    // If we failed in both the current region and the next free region 
    // then we know we can't allocate the object anywhere.
    print_heap_regions();
    tty->print_cr("free regions:");
    _free_regions->print();
    assert(false, "Failed to allocate object");
  }
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {

  // This was used for allocation while holding the Heap_lock.
  // HeapWord* filler = allocate_memory(BROOKS_POINTER_OBJ_SIZE + size);
  HeapWord* filler = allocate_memory_gclab(BROOKS_POINTER_OBJ_SIZE + size);

  HeapWord* result = filler + BROOKS_POINTER_OBJ_SIZE;
  if (filler != NULL) {
    initialize_brooks_ptr(filler, result);
    _bytesAllocSinceCM += size;
    _current_region->increase_live_data((size + BROOKS_POINTER_OBJ_SIZE) * HeapWordSize);
    if (ShenandoahGCVerbose)
      tty->print("mem_allocate_locked object of size %d at addr %p in epoch %d\n", size, result, _epoch);

    assert(! heap_region_containing(result)->is_dirty(), "never allocate in dirty region");
    return result;
  } else {
    tty->print_cr("Out of memory. Requested number of words: %x", size);
    print_heap_regions();
    tty->print("Printing %d free regions:\n", _free_regions->available_regions());
    _free_regions->print();

    assert(false, "Out of memory");
    return NULL;
  }
}

class PrintOopContents: public OopClosure {
public:
  void do_oop(oop* o) {
    oop obj = *o;
    tty->print("References oop "PTR_FORMAT"\n", obj);
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
    VM_ShenandoahVerifyHeap op(0, 0, GCCause::_allocation_failure);
    if (Thread::current()->is_VM_thread()) {
      op.doit();
    } else {
      // ...and get the VM thread to execute it.
      VMThread::execute(&op);
    }
  }
  _numAllocs++;
#endif

  // MutexLocker ml(Heap_lock);
  HeapWord* result = mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
  return result;
}

class ParallelEvacuateRegionObjectClosure : public ObjectClosure {
private:
  uint _epoch;
  ShenandoahHeap* _heap;
  ShenandoahAllocRegion* _region;
  size_t _waste;

  public:
  ParallelEvacuateRegionObjectClosure(uint epoch, 
				      ShenandoahHeap* heap, 
				      ShenandoahAllocRegion* allocRegion) :
    _epoch(epoch),
    _heap(heap),
    _region(allocRegion),
    _waste(0) { 
  }

  void verify_copy(oop p,oop c){
    assert(p != oopDesc::bs()->resolve_oop(p), "forwarded correctly");
    assert(oopDesc::bs()->resolve_oop(p) == c, "verify pointer is correct");
    if (p->klass() != c->klass()) {
      _heap->print_heap_regions();
    }
    assert(p->klass() == c->klass(), err_msg("verify class p-size: %d c-size: %d", p->size(), c->size()));
    assert(p->size() == c->size(), "verify size");
    assert(p->mark() == c->mark(), "verify mark");
    assert(c == oopDesc::bs()->resolve_oop(c), "verify only forwarded once");
  }

  void assign_brooks_pointer(oop p, HeapWord* filler, HeapWord* copy) {
    _heap->initialize_brooks_ptr(filler, copy);
    BrooksPointer::get(oop(copy)).set_age(BrooksPointer::get(p).get_age());
    BrooksPointer::get(p).set_forwardee(oop(copy));
    if (ShenandoahGCVerbose) {
      tty->print_cr("evacuating object: %p, of size %d with age %d, epoch %d to %p of size %d", 
                    p, p->size(), BrooksPointer::get(p).get_age(), _epoch, copy, oop(copy)->size());
      HandleMark hm;
      if (p->has_displaced_mark())
	tty->print("object has displaced mark\n");
      else {
	tty->print("previous object = \n");
	oop(p)->print();
	tty->print("copied object = \n");
	oop(copy)->print();
      }
    }
  }

  // Call this if you know we have enough space.
  void copy_object(oop p) {
    HeapWord* filler = _region->allocate(BROOKS_POINTER_OBJ_SIZE + p->size());
    copy_object(p, filler);
  }

  // Call this if we have already allocated the space.
  void copy_object(oop p, HeapWord* s) {
    HeapWord* filler = s;
    assert(s != NULL, "allocation of brooks pointer must not fail");
    HeapWord* copy = s + BROOKS_POINTER_OBJ_SIZE;
    assert(copy != NULL, "allocation of copy object must not fail");
    Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
    assign_brooks_pointer(p, filler, copy);

    if (ShenandoahGCVerbose) {
      tty->print_cr("copy object from %p to: %p epoch: %d, age: %d", p, copy, ShenandoahHeap::heap()->getEpoch(), BrooksPointer::get(p).get_age());
    }
#ifdef ASSERT
    if (ShenandoahVerify) {
      verify_copy(p, oop(copy));
    }
#endif
    if (p->has_displaced_mark())
      p->set_mark(p->displaced_mark());
  }    
  
  void do_object(oop p) {
    if ((! ShenandoahBarrierSet::is_brooks_ptr(p)) && BrooksPointer::get(p).get_age() == _epoch) {
      size_t required = BROOKS_POINTER_OBJ_SIZE + p->size();
      // tty->print("required = %d\n", required);

      if (required < _region->space_available()) {
	if (ShenandoahGCVerbose) 
	  tty->print("required < _region->space_available() = %d\n", _region->space_available());
	copy_object(p);
      } else if (required < _region->region_size()) {
	if (ShenandoahGCVerbose) 
	  tty->print("required < _region->region_size = %d\n ", _region->region_size());
	_waste += _region->space_available();
	_region->fill_region();
	_region->allocate_new_region();
	copy_object(p);
      } else if (required < ShenandoahHeapRegion::RegionSizeBytes) {
	if (ShenandoahGCVerbose) 
	  tty->print("required < ShenandoahHeapRegion::RegionSizeBytes = %d\n ", ShenandoahHeapRegion::RegionSizeBytes);
	_waste += _region->space_available();
        _region->fill_region();
	HeapWord* s = _heap->allocate_memory_gclab(required);
	copy_object(p, s);
      } else {
	assert(false, "Don't handle humongous objects yet");
      }
    }
  }
  size_t wasted() { return _waste;}
};
      

void ShenandoahHeap::initialize_brooks_ptr(HeapWord* filler, HeapWord* obj) {
  CollectedHeap::fill_with_array(filler, BROOKS_POINTER_OBJ_SIZE, false, false);
  markOop mark = oop(filler)->mark();
  oop(filler)->set_mark(mark->set_age(15));
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop(filler)), "brooks pointer must be brooks pointer");
  BrooksPointer::get(oop(obj)).set_forwardee(oop(obj));
}

class VerifyEvacuatedObjectClosure : public ObjectClosure {
  uint _epoch;

public:
  VerifyEvacuatedObjectClosure(uint epoch) : _epoch(epoch) {}
  
  void do_object(oop p) {
    if ((! ShenandoahBarrierSet::is_brooks_ptr(p)) && BrooksPointer::get(p).get_age() == _epoch) {
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

  VerifyEvacuatedObjectClosure verify_evacuation(_epoch);
  from_region->object_iterate(&verify_evacuation);
}

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region, 
					      ShenandoahAllocRegion *alloc_region) {
  ParallelEvacuateRegionObjectClosure evacuate_region(_epoch, this, alloc_region);
  if (ShenandoahGCVerbose) 
    tty->print("parallel_evacuate_region starting from_region %d: free_regions = %d\n",  from_region->regionNumber, _free_regions->available_regions());
  from_region->object_iterate(&evacuate_region);
  from_region->set_dirty(true);
#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_evacuated_region(from_region);
  }
#endif
  if (ShenandoahGCVerbose)
    tty->print("parallel_evacuate_region after from_region = %d: Wasted %d bytes free_regions = %d\n", from_region->regionNumber, evacuate_region.wasted(), _free_regions->available_regions());
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
     		   from_hr->regionNumber);
	from_hr->print();
      }

      // Not sure if the check is worth it or not.
      if (from_hr->getLiveData() != 0) {
	_sh->parallel_evacuate_region(from_hr, &allocRegion);
      } else {
	// We don't need to evacuate anything, but we still need to mark it dirty.
	from_hr->set_dirty(true);
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

    if (r->is_dirty()) {
      // tty->print_cr("recycling region %d:", r->regionNumber);
      // r->print_on(tty);
      // tty->print_cr("");
      r->recycle();
      r->set_dirty(false);
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
        tty->print_cr("%s (e%d) %d (%p)-> %p (age: %d) (%s %p)", _prefix, ShenandoahHeap::heap()->getEpoch(), _index, p, o, BrooksPointer::get(o).get_age(), o->klass()->internal_name(), o->klass());
      } else {
        tty->print_cr("%s (e%d) %d (%p dirty: %d) -> %p (not in heap, possibly corrupted or dirty (%d))", _prefix, ShenandoahHeap::heap()->getEpoch(), _index, p, ShenandoahHeap::heap()->heap_region_containing(p)->is_dirty(), o, ShenandoahHeap::heap()->heap_region_containing(o)->is_dirty());
      }
    } else {
      tty->print_cr("%s (e%d) %d (%p) -> %p", _prefix, ShenandoahHeap::heap()->getEpoch(), _index, p, o);
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
    //if (ShenandoahHeap::heap()->isMarkedCurrent(p)) {
    tty->print_cr("%s (e%d) object %p (age: %d) (%s %p) refers to:", _prefix, ShenandoahHeap::heap()->getEpoch(), p, BrooksPointer::get(p).get_age(), p->klass()->internal_name(), p->klass());
      PrintAllRefsOopClosure cl(_prefix);
      p->oop_iterate(&cl);
      //}
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
        tty->print_cr("oop not marked, although referrer is marked: %p: in_heap: %d, age: %d, epoch: %d", o, _heap->is_in(o), BrooksPointer::get(o).get_age(), _heap->getEpoch());
        tty->print_cr("oop class: %s", o->klass()->internal_name());
	if (_heap->is_in(p)) {
	  oop referrer = oop(_heap->heap_region_containing(p)->block_start_const(p));
	  tty->print("Referrer starts at addr %p\n", referrer);
	  referrer->print();
	}
	_heap->heap_region_containing(o)->print();
	_heap->heap_region_containing(p)->print();
	
      }
      assert(o->is_oop(), "oop must be an oop");
      assert(Metaspace::contains(o->klass()), "klass pointer must go to metaspace");
      assert(! ShenandoahBarrierSet::is_brooks_ptr(o), "oop must not be a brooks ptr");
      if (! ShenandoahBarrierSet::has_brooks_ptr(o)) {
        tty->print_cr("oop doesn't have a brooks ptr: %p", o);
      }
      assert(ShenandoahBarrierSet::has_brooks_ptr(o), "oop must have a brooks ptr");
      if (! (o == oopDesc::bs()->resolve_oop(o))) {
        tty->print_cr("oops has forwardee: p: %p (%d), o = %p (%d), new-o: %p (%d)", p, _heap->heap_region_containing(p)->is_dirty(), o,  _heap->heap_region_containing(o)->is_dirty(), oopDesc::bs()->resolve_oop(o),  _heap->heap_region_containing(oopDesc::bs()->resolve_oop(o))->is_dirty());
        tty->print_cr("oop class: %s", o->klass()->internal_name());
      }
      assert(o == oopDesc::bs()->resolve_oop(o), "oops must not be forwarded");
      assert(! _heap->heap_region_containing(o)->is_dirty(), "references must not point to dirty heap regions");
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
    if ((!ShenandoahBarrierSet::is_brooks_ptr(p)) && _heap->isMarkedCurrent(p)) {
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
    if ((!ShenandoahBarrierSet::is_brooks_ptr(p)) && _heap->isMarked(p)) {
      p->oop_iterate(_cl);
    }
  }

};

void ShenandoahHeap::verify_heap_after_marking() {
  VerifyAfterMarkingOopClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);
}

void ShenandoahHeap::parallel_evacuate() {

  if (ShenandoahGCVerbose) {
    tty->print_cr("starting parallel_evacuate");
    PrintHeapRegionsClosure pc1;
    heap_region_iterate(&pc1);
  }

#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_heap_after_marking();
  }
#endif

  RecycleDirtyRegionsClosure cl;
  heap_region_iterate(&cl);

  // _current_region->fill_region();
  _regions->choose_collection_set(_collection_set);
  _regions->choose_empty_regions(_free_regions);
  update_current_region();
  if (ShenandoahGCVerbose) {
    tty->print("Printing all available regions");
    print_heap_regions();
    tty->print("Printing collection set which contains %d regions:\n", _collection_set->available_regions());
    _collection_set->print();

    tty->print("Printing %d free regions:\n", _free_regions->available_regions());
    _free_regions->print();
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
  }

#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_heap_after_evacuation();
  }
#endif

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
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: %p", heap_oop));
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahHeap::roots_iterate(ExtendedOopClosure* cl) {

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

  assert((! is_in(p)) || (! heap_region_containing(p)->is_dirty()), "never update refs in from-space"); 

  oop heap_oop = *p; // read p
  if (! oopDesc::is_null(heap_oop)) {

    if (! is_in(heap_oop)) {
      print_heap_regions();
      tty->print_cr("object not in heap: %p, referenced by: %p", heap_oop, p);
      assert(is_in(heap_oop), "object must be in heap");
    }
    assert(is_in(heap_oop), "only ever call this on objects in the heap");
    assert(! (is_in(p) && heap_region_containing(p)->is_dirty()), "we don't want to update references in from-space");
    oop forwarded_oop = oopDesc::bs()->resolve_oop(heap_oop); // read brooks ptr
    if (forwarded_oop != heap_oop) {
      // tty->print_cr("updating old ref: %p pointing to %p to new ref: %p", p, heap_oop, forwarded_oop);
      assert(forwarded_oop->is_oop(), "oop required");
      assert(ShenandoahBarrierSet::has_brooks_ptr(forwarded_oop), "brooks pointer required");
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

size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  return 0;
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

void ShenandoahHeap::collect(GCCause::Cause) {
  // Unimplemented();
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  Unimplemented();
  return NULL;
  
}

ShenandoahCollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _pgc_policy;
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
  // Needed to keep going
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
    if (*p != NULL && ! ShenandoahBarrierSet::is_brooks_ptr(*p)) {
      oop heap_oop = oopDesc::load_heap_oop(p);
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(obj->is_oop(), "is_oop");
      /*
        { // Just for debugging.
        gclog_or_tty->print_cr("Root location "PTR_FORMAT" "
        "verified "PTR_FORMAT, p, (void*) obj);
        obj->print_on(gclog_or_tty);
        }
      */
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

    HeapWord* oopWord = (HeapWord*) p;
    if (ShenandoahBarrierSet::is_brooks_ptr(oop(oopWord))) { // Brooks pointer
      guarantee(arrayOop(oopWord)->length() == 2, "brooks ptr objects must have length == 2");
    } else {
      HeapWord* brooksPOop = (oopWord - BROOKS_POINTER_OBJ_SIZE);
      guarantee(ShenandoahBarrierSet::is_brooks_ptr(oop(brooksPOop)), "age in mark word of brooks obj must be 15");
    }
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
  return 0;
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

inline ShenandoahHeapRegion*
ShenandoahHeap::heap_region_containing(const void* addr) const {
  intptr_t region_start = ((intptr_t) addr) & ~(ShenandoahHeapRegion::RegionSizeBytes - 1);
  intptr_t index = (region_start - (intptr_t) _first_region->bottom()) / ShenandoahHeapRegion::RegionSizeBytes;
  return _ordered_regions[index];
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
  for (size_t i = 0; i < _numRegions; i++) {
    ShenandoahHeapRegion* current  = _ordered_regions[i];
    if ( !(skip_dirty_regions && current->is_dirty()) && blk->doHeapRegion(current)) 
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
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop(((HeapWord*) from_space_oop) - BROOKS_POINTER_OBJ_SIZE)), "oop must have a brooks ptr");
  return from_space_oop;
}
 */

ShenandoahMarkRefsClosure::ShenandoahMarkRefsClosure(uint e, uint worker_id) :
  _epoch(e), _worker_id(worker_id), _heap(ShenandoahHeap::heap()) {
}

void ShenandoahMarkRefsClosure::do_oop_work(oop* p) {

  // tty->print_cr("marking oop ref: %p", p);
  // We piggy-back reference updating to the marking tasks.
  oop* old = p;
  oop obj = _heap->maybe_update_oop_ref(p);

  if (ShenandoahGCVerbose)
    tty->print("Update %p => %p  to %p => %p\n", p, *p, old, *old);

  // NOTE: We used to assert the following here. This does not always work because
  // a concurrent Java thread could change the the field after we updated it.
  // oop obj = oopDesc::load_heap_oop(p);
  // assert(oopDesc::bs()->resolve_oop(obj) == *p, "we just updated the referrer");
  // assert(obj == NULL || ! _heap->heap_region_containing(obj)->is_dirty(), "must not point to dirty region");

  ShenandoahMarkObjsClosure cl(_epoch, _worker_id);
  cl.do_object(obj);
}

void ShenandoahMarkRefsClosure::do_oop(narrowOop* p) {
  assert(false, "narrowOops not supported");
}

void ShenandoahMarkRefsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

ShenandoahMarkObjsClosure::ShenandoahMarkObjsClosure(uint e, uint worker_id) : _epoch(e), _worker_id(worker_id) {
}

void ShenandoahMarkObjsClosure::do_object(oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap* ) Universe::heap();
  if (obj != NULL) {
    // TODO: The following resolution of obj is only ever needed when draining the SATB queues.
    // Wrap this closure to avoid this call in usual marking.
    obj = oopDesc::bs()->resolve_oop(obj);
    assert(! sh->heap_region_containing(obj)->is_dirty(), "we don't want to mark objects in from-space");
    assert(sh->is_in(obj), "referenced objects must be in the heap. No?");
    if (! sh->isMarkedCurrent(obj)) {
      /*
      if (obj->is_a(SystemDictionary::Throwable_klass())) {
        tty->print_cr("marking Throwable object %p, %d, %d", obj, getMark(obj)->age(), sh->getEpoch());
      }
      */
      sh->mark_current(obj);

      // Calculate liveness of heap region containing object.
      ShenandoahHeapRegion* region = sh->heap_region_containing(obj);
      region->increase_live_data((obj->size() + BROOKS_POINTER_OBJ_SIZE) * HeapWordSize);

      sh->concurrentMark()->addTask(obj, _worker_id);
    }
    /*
    else {
      tty->print_cr("already marked object %p, %d, %d", obj, getMark(obj)->age(), sh->getEpoch());
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
  ShenandoahMarkRefsClosure rootsCl(_epoch, 0);
  roots_iterate(&rootsCl);
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

class BumpObjectAgeClosure : public ObjectClosure {
  ShenandoahHeap* sh;
public:
  BumpObjectAgeClosure(ShenandoahHeap* heap) : sh(heap) { }
  
  void do_object(oop obj) {
    if ((! ShenandoahBarrierSet::is_brooks_ptr(obj))) {
      if (sh->isMarkedCurrent(obj)) {
        // tty->print_cr("bumping object to age 0: %p", obj);
        BrooksPointer::get(obj).set_age(0);
      }
      /*
        else {
        tty->print_cr("not bumping object to age 0: %p", obj);
        }
      */
      assert(! sh->isMarkedCurrent(obj), "no objects should be marked current after bumping");
    }
  }
};

void ShenandoahHeap::start_concurrent_marking() {
  set_concurrent_mark_in_progress(true);
  
  if (ShenandoahGCVerbose) 
    print_all_refs("pre -mark");

  // Increase and wrap epoch back to 1 (not 0!)
  _epoch = _epoch % MAX_EPOCH + 1;
  assert(_epoch > 0 && _epoch <= MAX_EPOCH, err_msg("invalid epoch: %d", _epoch));
  
  if (ShenandoahGCVerbose) {
    tty->print_cr("epoch = %d", _epoch);
    print_all_refs("pre-mark0");
  }

#ifdef ASSERT
  if (ShenandoahVerify) {
    BumpObjectAgeClosure boc(this);
    object_iterate(&boc);
  }
#endif
  
  ClearLivenessClosure clc(this);
  heap_region_iterate(&clc);

  // print_all_refs("pre -mark");

  // oopDesc::_debug = true;
  prepare_unmarked_root_objs();
  // if (getEpoch() < 5) {
  // oopDesc::_debug = false;
    //}
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
      guarantee(_sh->heap_region_containing(obj)->is_dirty() == (obj != oopDesc::bs()->resolve_oop(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %d, is_forwarded: %d",
                        _sh->heap_region_containing(obj)->is_dirty(),
                        obj != oopDesc::bs()->resolve_oop(obj))
                );
      obj = oopDesc::bs()->resolve_oop(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_dirty(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (! sh->isMarked(obj)) {
        sh->print_on(tty);
      }
      assert(sh->isMarked(obj), err_msg("Referenced Objects should be marked obj: %p, epoch: %d, obj-age: %d, is_in_heap: %d", 
					obj, sh->getEpoch(), BrooksPointer::get(obj).get_age(), sh->is_in(obj)));
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
      guarantee(_sh->heap_region_containing(obj)->is_dirty() == (obj != oopDesc::bs()->resolve_oop(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %d, is_forwarded: %d",
                        _sh->heap_region_containing(obj)->is_dirty(),
                        obj != oopDesc::bs()->resolve_oop(obj))
                );
      obj = oopDesc::bs()->resolve_oop(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_dirty(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      guarantee(Metaspace::contains(obj->klass()), "klass pointer must go to metaspace");

      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (! sh->isMarked(obj)) {
        sh->print_on(tty);
      }
      assert(sh->isMarked(obj), err_msg("Referenced Objects should be marked obj: %p, epoch: %d, obj-age: %d, is_in_heap: %d", 
                                        obj, sh->getEpoch(), BrooksPointer::get(obj).get_age(), sh->is_in(obj)));
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_heap_after_evacuation() {

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
}

bool ShenandoahHeap::should_start_concurrent_marking() {
  return ! Atomic::cmpxchg(true, &_concurrent_mark_in_progress, false);
}

bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}

bool ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  _concurrent_mark_in_progress = in_progress;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, ! in_progress);
  JavaThread::update_refs_queue_set().set_active_all_threads(in_progress, ! in_progress);
}

void ShenandoahHeap::post_allocation_collector_specific_setup(HeapWord* hw) {
  oop obj = oop(hw);

  // Assuming for now that objects can't be created already locked
  assert(! obj->has_displaced_mark(), "hopefully new objects don't have displaced mark");
  // tty->print_cr("post_allocation_collector_specific_setup:: %p, (%d)", obj, _epoch);

  mark_current_no_checks(obj);

}

void ShenandoahHeap::mark_current(oop obj) const {
  assert(_epoch > 0 && _epoch <= MAX_EPOCH, err_msg("invalid epoch: %d", _epoch));
  assert(obj == oopDesc::bs()->resolve_oop(obj), "only mark forwarded copy of objects");
  mark_current_no_checks(obj);
}

void ShenandoahHeap::mark_current_no_checks(oop obj) const {
  BrooksPointer::get(obj).set_age(_epoch);
}

bool ShenandoahHeap::isMarkedPrev(oop obj) const {
  assert(_epoch > 0, "invalid epoch");
  uint previous_epoch = _epoch - 1;
  if (previous_epoch == 0) {
    previous_epoch = MAX_EPOCH;
  }
  return BrooksPointer::get(obj).get_age() == previous_epoch;
}

bool ShenandoahHeap::isMarkedCurrent(oop obj) const {
  // tty->print_cr("obj age: %d", BrooksPointer::get(obj).get_age());
  return BrooksPointer::get(obj).get_age() == _epoch;
}
  

#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "utilities/copy.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"

ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

markOop getMark(oop obj) {
  if (obj->has_displaced_mark())
    return obj->displaced_mark();
  else
    return obj->mark();
}

void setMark(oop obj, markOop mark) {
  if (obj->has_displaced_mark()) {
    obj->set_displaced_mark(mark);
  } else {
    obj->set_mark(mark);
  }
}

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
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print("Region %d bottom = "PTR_FORMAT" end = "PTR_FORMAT" top = "PTR_FORMAT" used = %x free = %x live = %x dirty: %d\n", 
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

  tty->print("Calling initialize on reserved space base = %p end = %p\n", 
	     pgc_rs.base(), pgc_rs.base() + pgc_rs.size());

  _numRegions = init_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _numRegions, mtGC); 

  ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
  _current_region = current;
  _first_region = current;
  _initialSize = _numRegions * ShenandoahHeapRegion::RegionSizeBytes;
  size_t regionSizeWords = ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
  assert(init_byte_size == _initialSize, "tautology");
  _regions = new ShenandoahHeapRegionSet(_numRegions);
  _free_regions = new ShenandoahHeapRegionSet(_numRegions);
  _collection_set = new ShenandoahHeapRegionSet(_numRegions);

  regions[0] = current;

  for (size_t i = 0; i < _numRegions - 1; i++) {
    ShenandoahHeapRegion* next = new ShenandoahHeapRegion();
    current->initialize((HeapWord*) pgc_rs.base() + 
			regionSizeWords * i, regionSizeWords);
    current->setNext(next);
    current->regionNumber = i;
    _regions->put(i, current);
    _free_regions->put(i, current);
    current = next;
    regions[i+1] = current;
  }

  size_t last_region = _numRegions - 1;
  current->initialize((HeapWord*) pgc_rs.base() + regionSizeWords * (last_region), 
		      regionSizeWords);
  current->setNext(NULL);

  current->regionNumber = last_region;
  _regions->put(last_region, current);
  _free_regions->put(last_region, current);
  _numAllocs = 0;

  tty->print("All Regions\n");
  _regions->print();
  tty->print("Free Regions\n");
  _free_regions->print();

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

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
    if (r->next() == NULL)
      return true;
    else return false;
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
  IsInRegionClosure isIn(p);
  heap_region_iterate(&isIn);
  bool result = isIn.result();
  
  return isIn.result();
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
  bool* limit_exceeded;
  return mem_allocate(word_size, limit_exceeded);
}


HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
  bool* limit_exceeded;
  HeapWord* result = _current_region->allocate(word_size);
  if (!result) {
    update_current_region();
    return allocate_new_gclab(word_size);
  } else {
    _current_region->increase_live_data((jlong)word_size);
    return result;
  }
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
     tty->print("Old current region = \n");
     _current_region->print();
  }
   
  if (_free_regions->has_next()) {
       _current_region = _free_regions->get_next();
  } else {
    if (ShenandoahGCVerbose) {
       PrintHeapRegionsClosure pc;
       heap_region_iterate(&pc);
    }
    assert(false, "No GC implemented");
  }

  if (ShenandoahGCVerbose) {
    tty->print("New current region = \n");
    _current_region->print();
  }
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {
  if (_current_region->is_dirty() || (_current_region == NULL))
    update_current_region();

  HeapWord* filler = _current_region->allocate(BROOKS_POINTER_OBJ_SIZE);
  HeapWord* result = NULL;
  if (filler != NULL) {
    result = _current_region->allocate(size);
    if (result != NULL) {
      initialize_brooks_ptr(filler, result);
      _bytesAllocSinceCM+= size;
      _current_region->setLiveData(_current_region->used());
      return result;
    } else {
      _current_region->rollback_allocation(BROOKS_POINTER_OBJ_SIZE);
    }
  }
  assert (result == NULL, "expect result==NULL");
  update_current_region();
  return mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
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

#ifdef SLOWDEBUG
  if (_numAllocs > 1000000) {
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


  // These are just arbitrary numbers for now.  CHF
  size_t targetStartMarking = capacity() / 5;
  size_t targetBytesAllocated = ShenandoahHeapRegion::RegionSizeBytes;

  if (used() > targetStartMarking && _bytesAllocSinceCM > targetBytesAllocated && should_start_concurrent_marking()) {
    _bytesAllocSinceCM = 0;
    tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT" Target = "SIZE_FORMAT" doing initMark\n", capacity(), used(), targetStartMarking);
    mark();

  }
  
  MutexLocker ml(Heap_lock);
  HeapWord* result = mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
  return result;
}

void ShenandoahHeap::mark() {

    tty->print("Starting a mark");

    VM_ShenandoahInitMark initMark;
    VMThread::execute(&initMark);
    
    concurrentMark()->markFromRoots();

    VM_ShenandoahFinishMark finishMark;
    VMThread::execute(&finishMark);

#ifdef SLOWDEBUG   
    verify_liveness_after_concurrent_mark();
#endif
}

class SelectEvacuationRegionsClosure : public ShenandoahHeapRegionClosure {

private:
  ShenandoahHeapRegion* _empty_region;
  ShenandoahHeapRegion* _evacuation_region;
  size_t _most_garbage;
public:
  SelectEvacuationRegionsClosure() : _empty_region(NULL), _evacuation_region(NULL), _most_garbage(0) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    // We piggy-back clearing the heap regions here because it's convenient.
    r->set_dirty(false);
    if (r->garbage() > _most_garbage) {
      _evacuation_region = r;
      _most_garbage = r->garbage();
    }
    if (_empty_region == NULL && r->used() == 0) {
      _empty_region = r;
    }

    // TODO: Maybe use smarter heuristic above and stop earlier?
    if (r->next() == NULL)
      return true;
    else return false;
  }


  ShenandoahHeapRegion* evacuation_region() {
    return _evacuation_region;
  }

  bool found_evacuation_region() {
    if (_evacuation_region == NULL)
      return false;
    else 
      return true;
  }

  ShenandoahHeapRegion* empty_region() {
    return _empty_region;
  }

  bool found_empty_region() {
    if (_empty_region == NULL)
      return false;
    else 
      return true;
  }
};

class EvacuateRegionObjectClosure: public ObjectClosure {
private:
  uint _epoch;
  ShenandoahHeap* _heap;
  ShenandoahHeapRegion* _to_region;
public:
  EvacuateRegionObjectClosure(uint epoch, ShenandoahHeap* heap, ShenandoahHeapRegion* to_region) :
    _epoch(epoch),
    _heap(heap),
    _to_region(to_region) {};

  void do_object(oop p) {
    // if (! ShenandoahBarrierSet::is_brooks_ptr(p)) { // Copy everything except brooks ptrs.
    if (_heap->isMarkedCurrent(p)) { // Ignores brooks ptr objects because epoch is never 15.
      // Allocate brooks ptr object for copy.
      HeapWord* filler = _to_region->allocate(BROOKS_POINTER_OBJ_SIZE);
      assert(filler != NULL, "brooks ptr for copied object must not be NULL");
      // Copy original object.
      HeapWord* copy = _to_region->allocate(p->size());
      assert(copy != NULL, "allocation of copy object must not fail");
      Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
      _heap->initialize_brooks_ptr(filler, copy);
      HeapWord* old_brooks_ptr = ((HeapWord*) p) - BROOKS_POINTER_OBJ_SIZE;
      // tty->print_cr("setting old brooks ptr: p: %p, old_brooks_ptr: %p to copy: %p", p, old_brooks_ptr, copy);
      if (ShenandoahGCVerbose) {
	        tty->print_cr("evacuating object: %p, with brooks ptr %p and age %d, epoch %d to %p with brooks ptr %p", 
			      p, old_brooks_ptr, getMark(p)->age(), _epoch, copy, filler);
      }

      _heap->set_brooks_ptr(old_brooks_ptr, copy);
    }
    /*
    else {
      tty->print_cr("not evacuating object: %p, %d, %d", p, getMark(p)->age(), _epoch);
    }
    */
  }
};

class ParallelEvacuateRegionObjectClosure : public ObjectClosure {
private:
  uint _epoch;
  ShenandoahHeap* _heap;
  ShenandoahAllocRegion _region;
  size_t _waste;

  public:
  ParallelEvacuateRegionObjectClosure(uint epoch, 
				      ShenandoahHeap* heap, 
				      ShenandoahAllocRegion allocRegion) :
    _epoch(epoch),
    _heap(heap),
    _region(allocRegion),
    _waste(0) { 
  }

  void copy_object(oop p) {
    HeapWord* filler = _region.allocate(BROOKS_POINTER_OBJ_SIZE + p->size());
    assert(filler != NULL, "brooks ptr for copied object must not be NULL");
	// Copy original object.
    HeapWord* copy = filler + BROOKS_POINTER_OBJ_SIZE;
    assert(copy != NULL, "allocation of copy object must not fail");
    Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
    _heap->initialize_brooks_ptr(filler, copy);
    HeapWord* old_brooks_ptr = ((HeapWord*) p) - BROOKS_POINTER_OBJ_SIZE;
    _heap->set_brooks_ptr(old_brooks_ptr, copy);
    oop c = oop(copy);
    if (ShenandoahGCVerbose) {
      tty->print_cr("evacuating object: %p, of size %d with age %d, epoch %d to %p of size %d", 
		    p, p->size(), getMark(p)->age(), _epoch, copy, oop(copy)->size());
    }

    assert(p != oopDesc::bs()->resolve_oop(p), "forwarded correctly");
    assert(oopDesc::bs()->resolve_oop(p) == c, "verify pointer is correct");
    assert(p->klass() == c->klass(), "verify class");
    assert(p->size() == c->size(), "verify size");
    assert(p->mark() == c->mark(), "verify mark");
    assert(c == oopDesc::bs()->resolve_oop(c), "verify only forwarded once");
  }
  
  void do_object(oop p) {
    if (getMark(p)->age() == _epoch) {
      size_t required = BROOKS_POINTER_OBJ_SIZE + p->size();
      if (required < _region.space_available()) {
	copy_object(p);
      } else if (required < _region.region_size()) {
	_waste += _region.space_available();
	_region.fill_region();
	_region.allocate_new_region();
	copy_object(p);
      } else if (required < ShenandoahHeapRegion::RegionSizeBytes) {
	_waste += _region.space_available();
	_region.fill_region();
	_heap->current_region()->fill_region();
	_heap->update_current_region();
	_region.allocate_new_region();
	copy_object(p);
      } else {
	assert(false, "Don't handle humongous objects yet");
      }
    }
  }
  size_t wasted() { return _waste;}
};
      

void ShenandoahHeap::set_brooks_ptr(HeapWord* brooks_ptr, HeapWord* obj) {
  // Set the brooks pointer
  HeapWord* first = brooks_ptr + (BROOKS_POINTER_OBJ_SIZE - 1);
  uintptr_t first_ptr = (uintptr_t) first;
  *(unsigned long*)(((unsigned long*)first_ptr)) = (unsigned long) obj;
  //tty->print_cr("result, brooks obj, brooks ptr: %p, %p, %p", obj, filler, first);

}

void ShenandoahHeap::initialize_brooks_ptr(HeapWord* filler, HeapWord* obj) {
  CollectedHeap::fill_with_array(filler, BROOKS_POINTER_OBJ_SIZE, false);
  markOop mark = oop(filler)->mark();
  oop(filler)->set_mark(mark->set_age(15));
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop(filler)), "brooks pointer must be brooks pointer");
  arrayOop(filler)->set_length(1);
  set_brooks_ptr(filler, obj);
}

void ShenandoahHeap::evacuate_region(ShenandoahHeapRegion* from_region, ShenandoahHeapRegion* to_region) {

  if (ShenandoahGCVerbose) {
    MutexLocker ml(Heap_lock);
    tty->print_cr("evacuate region %d with garbage: %d, to empty region with used: %d", from_region->regionNumber, from_region->garbage(), to_region->used());
    tty->print_cr("from region: " );
    from_region->print_on(tty);
    tty->print_cr("\nto region");
    to_region->print_on(tty);
  }

  EvacuateRegionObjectClosure evacuate_region(_epoch, this, to_region);
  from_region->object_iterate(&evacuate_region);
  from_region->set_dirty(true);
}

class VerifyEvacuatedObjectClosure : public ObjectClosure {
  uint _epoch;

public:
  VerifyEvacuatedObjectClosure(uint epoch) : _epoch(epoch) {}
  
  void do_object(oop p) {
    if (p->mark()->age() == _epoch) {
      oop p_prime = oopDesc::bs()->resolve_oop(p);
      assert(p != p_prime, "Should point to evacuated copy");
      assert(p->klass() == p_prime->klass(), "Should have the same class");
      assert(p->mark() == p_prime->mark(), "Should have the same mark");
      assert(p->size() == p_prime->size(), "Should be the same size");
      assert(p_prime == oopDesc::bs()->resolve_oop(p_prime), "One forward once");
    }
  }
};
    
void ShenandoahHeap::verify_evacuated_region(ShenandoahHeapRegion* from_region) {
  tty->print("Verifying From Region\n");
  from_region->print();

  VerifyEvacuatedObjectClosure verify_evacuation(_epoch);
  from_region->object_iterate(&verify_evacuation);
}

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region, 
					      ShenandoahAllocRegion alloc_region) {
  ParallelEvacuateRegionObjectClosure evacuate_region(_epoch, this, alloc_region);
  from_region->object_iterate(&evacuate_region);
  from_region->set_dirty(true);
  verify_evacuated_region(from_region);
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
	_sh->parallel_evacuate_region(from_hr, allocRegion);
      } else {
	// We don't need to evacuate anything, but we still need to mark it dirty.
	from_hr->set_dirty(true);
      }

      from_hr = _cs->claim_next();
    }
    allocRegion.fill_region();
    tty->print("Thread %d entering barrier sync\n", worker_id);
    _barrier_sync->enter();
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

void ShenandoahHeap::parallel_evacuate() {

  if (ShenandoahGCVerbose) {
    tty->print_cr("starting parallel_evacuate");
    PrintHeapRegionsClosure pc1;
    heap_region_iterate(&pc1);
  }

  RecycleDirtyRegionsClosure cl;
  heap_region_iterate(&cl);

  _current_region->fill_region();
  _regions->choose_collection_set(_collection_set);
  _regions->choose_empty_regions(_free_regions);
  
  tty->print("Printing collection set which contains %d regions:\n", _collection_set->available_regions());
  _collection_set->print();

  tty->print("Printing %d free regions:\n", _free_regions->available_regions());
  _free_regions->print();

  barrierSync.set_n_workers(_max_workers);
  
  ParallelEvacuationTask evacuationTask = ParallelEvacuationTask(this, _collection_set, &barrierSync);

  workers()->run_task(&evacuationTask);
  
  
  if (ShenandoahGCVerbose) {
    tty->print_cr("finished parallel_evacuate");
    PrintHeapRegionsClosure pc2;
    heap_region_iterate(&pc2);
  }
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

void ShenandoahHeap::verify_evacuation(ShenandoahHeapRegion* from_region) {

  VerifyEvacuationClosure rootsCl(from_region);
  CodeBlobToOopClosure blobsCl(&rootsCl, false);
  KlassToOopClosure klassCl(&rootsCl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  ClassLoaderDataGraph::clear_claimed_marks();

  process_strong_roots(true, false, ScanningOption(so), &rootsCl, &blobsCl, &klassCl);

  //oop_iterate(&rootsCl);

}

void ShenandoahHeap::maybe_update_oop_ref(oop* p) {
  oop heap_oop = *p;
  if (! oopDesc::is_null(heap_oop)) {
    oop forwarded_oop = oopDesc::bs()->resolve_oop(heap_oop);
    if (forwarded_oop != heap_oop) {
      // tty->print_cr("updating old ref: %p to new ref: %p", heap_oop, forwarded_oop);
      *p = forwarded_oop;
      assert(*p == forwarded_oop, "make sure to update reference correctly");
    }
    /*
      else {
      tty->print_cr("not updating ref: %p", heap_oop);
      }
    */
  }
}

class UpdateRefsAfterEvacuationClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
public:
  UpdateRefsAfterEvacuationClosure() :
    _heap(ShenandoahHeap::heap()) { }

  void do_oop(oop* p) {
    _heap->maybe_update_oop_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

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
  assert(false, "Shouldn't need to do full collections");
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
  Unimplemented();
  return 0;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  Unimplemented();
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
      guarantee(arrayOop(oopWord)->length() == 1, "brooks ptr objects must have length == 1");
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

    CodeBlobToOopClosure blobsCl(&rootsCl, /*do_marking=*/ false);
    ShenandoahVerifyKlassClosure klassCl(&rootsCl);

    // We apply the relevant closures to all the oops in the
    // system dictionary, the string table and the code cache.
    const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

    // Need cleared claim bits for the strong roots processing
    ClassLoaderDataGraph::clear_claimed_marks();

    process_strong_roots(true,      // activate StrongRootsScope
			 false,     // we set "is scavenging" to false,
			 // so we don't reset the dirty cards.
			 ScanningOption(so),  // roots scanning options
			 &rootsCl,
			 &blobsCl,
			 &klassCl
			 );

    bool failures = rootsCl.failures();
    gclog_or_tty->print("verify failures: %d", failures); 

    ShenandoahVerifyHeapClosure heapCl(rootsCl);

    object_iterate(&heapCl);
    // TODO: Implement rest of it.
    verify_live();
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

template<class T>
inline ShenandoahHeapRegion*
ShenandoahHeap::heap_region_containing(const T addr) const {
  intptr_t region_start = ((intptr_t) addr) & ~(ShenandoahHeapRegion::RegionSizeBytes - 1);
  intptr_t index = (region_start - (intptr_t) _first_region->bottom()) / ShenandoahHeapRegion::RegionSizeBytes;
  return regions[index];
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
  ShenandoahHeapRegion* current  = _first_region;
  while (current != NULL && ((skip_dirty_regions && current->is_dirty()) || !blk->doHeapRegion(current))) {
    current = current->next();
  }
}

class IsInReservedClosure : public ShenandoahHeapRegionClosure {
  const void* _p;
  bool _result;
public:

  IsInReservedClosure(const void* p) {
    _p = p;
    _result = false;
  }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_in_reserved(_p)) {
      _result = true;
      return true;
    }
    if (r->next() == NULL) 
      return true;
    else return false;
  }

  bool result() { return _result;}
};
 

 bool ShenandoahHeap::is_in_reserved(void* p) {
   IsInReservedClosure blk(p);
   heap_region_iterate(&blk);
 }



ShenandoahMarkRefsClosure::ShenandoahMarkRefsClosure(uint e, uint worker_id) :
  _epoch(e), _worker_id(worker_id), _heap(ShenandoahHeap::heap()) {
}

void ShenandoahMarkRefsClosure::do_oop_work(oop* p) {

  // We piggy-back reference updating to the marking tasks.
  _heap->maybe_update_oop_ref(p);

  oop obj = oopDesc::load_heap_oop(p);
  assert(obj == *p, "we just updated the referrer");
  ShenandoahMarkObjsClosure cl(_epoch, _worker_id);
  cl.do_object(obj);
}

void ShenandoahMarkRefsClosure::do_oop(narrowOop* p) {
  assert(false, "narrorOops not supported");
}

void ShenandoahMarkRefsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

ShenandoahMarkObjsClosure::ShenandoahMarkObjsClosure(uint e, uint worker_id) : _epoch(e), _worker_id(worker_id) {
}

void ShenandoahMarkObjsClosure::do_object(oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap* ) Universe::heap();
  if (obj != NULL && (sh->is_in(obj))) {
    if (! sh->isMarkedCurrent(obj)) {
      // tty->print_cr("marking root object %p, %d, %d", obj, getMark(obj)->age(), epoch);
      sh->mark_current(obj);

      // Calculate liveness of heap region containing object.
      ShenandoahHeapRegion* region = sh->heap_region_containing(obj);
      region->increase_live_data(obj->size() * HeapWordSize + (BROOKS_POINTER_OBJ_SIZE * HeapWordSize));

      sh->concurrentMark()->addTask(obj, _worker_id);
    }
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
  CodeBlobToOopClosure blobsCl(&rootsCl, false);
  KlassToOopClosure klassCl(&rootsCl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  ClassLoaderDataGraph::clear_claimed_marks();

  process_strong_roots(true, false, ScanningOption(so), &rootsCl, &blobsCl, &klassCl);
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
    if (sh->isMarkedCurrent(obj)) {
      // tty->print_cr("bumping object to age 0: %p", obj);
      markOop mark = getMark(obj);
      setMark(obj, mark->set_age(0));
    }
    /*
    else {
      tty->print_cr("not bumping object to age 0: %p", obj);
    }
    */
    assert(! sh->isMarkedCurrent(obj), "no objects should be marked current after bumping");
  }
};


void ShenandoahHeap::start_concurrent_marking() {
  set_concurrent_mark_in_progress(true);

  // Increase and wrap epoch back to 1 (not 0!)
  _epoch = _epoch % MAX_EPOCH + 1;
  assert(_epoch > 0 && _epoch <= MAX_EPOCH, err_msg("invalid epoch: %d", _epoch));
  tty->print_cr("epoch = %d", _epoch);

#ifdef SLOWDEBUG
  BumpObjectAgeClosure boc(this);
  object_iterate(&boc);
#endif
  
  ClearLivenessClosure clc(this);
  heap_region_iterate(&clc);

  prepare_unmarked_root_objs();
}


class VerifyLivenessChildClosure : public ExtendedOopClosure {
 private:

   template<class T> void do_oop_nv(T* p) {
   T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      assert(sh->isMarked(obj), "Referenced Objects should be marked");
    }
   }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

class VerifyLivenessParentClosure : public ExtendedOopClosure {
private:

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (sh->isMarked(obj)) {
 	VerifyLivenessChildClosure childClosure;
 	obj->oop_iterate(&childClosure);
      }
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_live() {
  VerifyLivenessParentClosure verifyLive;
  oop_iterate(&verifyLive, true, true);
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
}

void ShenandoahHeap::post_allocation_collector_specific_setup(HeapWord* hw) {
  oop obj = oop(hw);

  // Assuming for now that objects can't be created already locked
  assert(! obj->has_displaced_mark(), "hopefully new objects don't have displaced mark");
  setMark(obj, getMark(obj)->set_age(_epoch));

}

void ShenandoahHeap::mark_current(oop obj) const {
  setMark(obj, getMark(obj)->set_age(_epoch));
}

bool ShenandoahHeap::isMarkedPrev(oop obj) const {
  assert(_epoch > 0, "invalid epoch");
  uint previous_epoch = _epoch - 1;
  if (previous_epoch == 0) {
    previous_epoch = MAX_EPOCH;
  }
  return getMark(obj)->age() == previous_epoch;
}

bool ShenandoahHeap::isMarkedCurrent(oop obj) const {
  return getMark(obj)->age() == _epoch;
}
  
class VerifyLivenessAfterConcurrentMarkChildClosure : public ExtendedOopClosure {

 private:

   template<class T> void do_oop_nv(T* p) {
   T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (ShenandoahGCVerbose) {
	if (obj->has_displaced_mark()) 
	  tty->print("Verifying liveness of reference obj "PTR_FORMAT" with displaced mark %x\n", 
		     obj, getMark(obj)->age());
	else tty->print("Verifying liveness of reference obj "PTR_FORMAT" with mark %x\n", 
			obj, getMark(obj)->age());
	obj->print();
      }
      /*
      if (! sh->isMarkedCurrent(obj)) {
        tty->print_cr("obj should be marked: %p, forwardee: %p", obj, oopDesc::bs()->resolve_oop(obj));
      }
      */
      assert(sh->isMarkedCurrent(obj), err_msg("Referenced Objects should be marked obj: %p, epoch: %d, obj-age: %d, is_in_heap: %d", obj, sh->getEpoch(), getMark(obj)->age(), sh->is_in(obj)));
    }
   }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

class VerifyLivenessAfterConcurrentMarkParentClosure : public ExtendedOopClosure {
private:

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (sh->isMarkedCurrent(obj)) {
	if (ShenandoahGCVerbose) {
	  if (obj->has_displaced_mark())
	    tty->print("Verifying liveness of objects pointed to by "PTR_FORMAT" with displaced mark %d\n", 
		       obj, getMark(obj)->age());
	  else tty->print("Verifying liveness of objects pointed to by "PTR_FORMAT" with mark %d\n", 
			  obj, getMark(obj)->age());
	  obj->print();
        }
 	VerifyLivenessAfterConcurrentMarkChildClosure childClosure;
 	obj->oop_iterate(&childClosure);
      }
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

// This should only be called after we finish concurrent mark before we start up mutator threads again.

void ShenandoahHeap::verify_liveness_after_concurrent_mark() {
  VerifyLivenessAfterConcurrentMarkParentClosure verifyLive;
  oop_iterate(&verifyLive, true, true);
}


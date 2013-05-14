#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSetChooser.hpp"
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
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x live = %x\n", 
	       r->regionNumber, r->top(), r->used(), r->free(), r->getLiveData());
    if (r->next() == NULL)
      return true;
    else return false;
  }
};

class PrintHeapObjectsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x\n", 
	       r->regionNumber, r->top(), r->used(), r->free());
    
    printHeapObjects(r->bottom(), r->top());
    if (r->next() == NULL)
      return true;
    else return false;
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

  tty->print("Calling initialize on reserved space base = %p end = %p\n", 
	     pgc_rs.base(), pgc_rs.base() + pgc_rs.size());
  
  ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
  firstRegion = current;
  currentRegion = firstRegion;
  numRegions = init_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  initialSize = numRegions * ShenandoahHeapRegion::RegionSizeBytes;
  size_t regionSizeWords = ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
  assert(init_byte_size == initialSize, "tautology");
  
  for (size_t i = 0; i < numRegions - 1; i++) {
    ShenandoahHeapRegion* next = new ShenandoahHeapRegion();
    current->initialize((HeapWord*) pgc_rs.base() + regionSizeWords * i, regionSizeWords);
    current->setNext(next);
    current->regionNumber = i;
    current = next;
  }
  current->initialize((HeapWord*) pgc_rs.base() + regionSizeWords * (numRegions - 1), 
		      regionSizeWords);
  current->setNext(NULL);
  current->regionNumber = numRegions - 1;

  //  PrintHeapRegionsClosure pc;
  //  heap_region_iterate(&pc);

  numAllocs = 0;
  //  _sct = new ShenandoahConcurrentThread();
  //  if (_sct == NULL)
  //    return JNI_ENOMEM;

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
  epoch(1),
  bytesAllocSinceCM(0) {
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
}


void ShenandoahHeap::nyi() const {
  assert(false, "not yet implemented");
  tty->print("not yet implmented\n");
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
  return numRegions * ShenandoahHeapRegion::RegionSizeBytes;

}

bool ShenandoahHeap::is_maximal_no_gc() const {
  nyi();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return numRegions * ShenandoahHeapRegion::RegionSizeBytes;
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
    if (r->next() == NULL) 
      return true;
    else return false;
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
  nyi();
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

public:

  FindEmptyRegionClosure() {
    _result = NULL;
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_empty()) {
      _result = r;
      return true;
    }
    if (r->next() == NULL) 
      return true;
    else return false;
  }
  ShenandoahHeapRegion* result() { return _result;}

};

ShenandoahHeapRegion* ShenandoahHeap::nextEmptyRegion() {
  FindEmptyRegionClosure cl;
  heap_region_iterate(&cl);
  return cl.result();
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {

   if (currentRegion == NULL) {
     tty->print("About to assert that no GC is implemented\n");
     PrintHeapRegionsClosure pc;
     heap_region_iterate(&pc);
     assert(false, "No GC implemented");
   }

   HeapWord* filler = currentRegion->allocate(BROOKS_POINTER_OBJ_SIZE);
   HeapWord* result = NULL;
   if (filler != NULL) {
     result = currentRegion->allocate(size);
     if (result != NULL) {
       initialize_brooks_ptr(filler, result);
       bytesAllocSinceCM+= size;
       return result;
     } else {
       currentRegion->rollback_allocation(BROOKS_POINTER_OBJ_SIZE);
     }
   }
   assert (result == NULL, "expect result==NULL");

   // tty->print("Closing region %d "PTR_FORMAT"["PTR_FORMAT":%d] and starting region %d "PTR_FORMAT" total used in bytes = "SIZE_FORMAT" M \n", 
   // 	      currentRegion->regionNumber, currentRegion, 
   // 	      currentRegion->top(), currentRegion->used(),
   // 	      currentRegion->next() != NULL ? currentRegion->next()->regionNumber : -1,
   // 	      currentRegion->next(),
   // 	      used_in_bytes()/M);

   /*
     printHeapObjects(currentRegion->bottom(), currentRegion->top());
   */
   currentRegion->setLiveData(currentRegion->used());

   currentRegion = nextEmptyRegion();
   if (currentRegion == NULL) {
     tty->print("About to assert that no GC is implemented\n");
     PrintHeapRegionsClosure pc;
     heap_region_iterate(&pc);
     assert(false, "No GC implemented");
   } else {
     return mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
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

  if (numAllocs > 1000000) {
    numAllocs = 0;
    VM_ShenandoahVerifyHeap op(0, 0, GCCause::_allocation_failure);
    if (Thread::current()->is_VM_thread()) {
      op.doit();
    } else {
      // ...and get the VM thread to execute it.
      VMThread::execute(&op);
    }
  }
  numAllocs++;

  // These are just arbitrary numbers for now.  CHF
  size_t targetStartMarking = capacity() / 4;
  size_t targetBytesAllocated = ShenandoahHeapRegion::RegionSizeBytes;
  if (ShenandoahGCVerbose) 
    tty->print("targetBytesAllocated = %d bytesAllocSinceCM = %d\n", targetBytesAllocated, bytesAllocSinceCM);

  if (used() > targetStartMarking && bytesAllocSinceCM > targetBytesAllocated && should_start_concurrent_marking()) {
    bytesAllocSinceCM = 0;
    tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT" Target = "SIZE_FORMAT" doing initMark\n", capacity(), used(), targetStartMarking);
    mark();

  }

  MutexLocker ml(Heap_lock);
  return mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
}

void ShenandoahHeap::mark() {

    epoch = (epoch + 1) % 8;
    tty->print("epoch = %d", epoch);
    VM_ShenandoahInitMark initMark;
    VMThread::execute(&initMark);
    
    concurrentMark()->markFromRoots();

    VM_ShenandoahFinishMark finishMark;
    VMThread::execute(&finishMark);
    
    verify_liveness_after_concurrent_mark();
}

class SelectEvacuationRegionsClosure : public ShenandoahHeapRegionClosure {

private:
  ShenandoahHeapRegion* _empty_region;
  ShenandoahHeapRegion* _evacuation_region;
  size_t _most_garbage;
public:
  SelectEvacuationRegionsClosure() : _empty_region(NULL), _evacuation_region(NULL), _most_garbage(0) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print_cr("used: %d, live: %d, garbage: %d", r->used(), r->getLiveData(), r->garbage());
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
    // if (! _heap->is_brooks_ptr(p)) { // Copy everything except brooks ptrs.
    if (getMark(p)->age() == _epoch) { // Ignores brooks ptr objects because epoch is never 15.
      // tty->print_cr("evacuating object: %p, %d, %d", p, getMark(p)->age(), _epoch);
      // Allocate brooks ptr object for copy.
      HeapWord* filler = _to_region->allocate(BROOKS_POINTER_OBJ_SIZE);
      assert(filler != NULL, "brooks ptr for copied object must not be NULL");
      // Copy original object.
      HeapWord* copy = _to_region->allocate(p->size());
      assert(copy != NULL, "allocation of copy object must not fail");
      Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
      _heap->initialize_brooks_ptr(filler, copy);
      HeapWord* old_brooks_ptr = ((HeapWord*) p) - BROOKS_POINTER_OBJ_SIZE;
      //tty->print_cr("setting old brooks ptr: p: %p, old_brooks_ptr: %p", p, old_brooks_ptr);
      _heap->set_brooks_ptr(old_brooks_ptr, copy);
    }
    /*
    else {
      tty->print_cr("not evacuating object: %p, %d, %d", p, getMark(p)->age(), _epoch);
    }
    */
  }
};

bool ShenandoahHeap::is_brooks_ptr(oop p) {
  if (p->has_displaced_mark())
    return false;
  return p->mark()->age() == 15;
}

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
  assert(is_brooks_ptr(oop(filler)), "brooks pointer must be brooks pointer");
  arrayOop(filler)->set_length(1);
  set_brooks_ptr(filler, obj);
}

void ShenandoahHeap::evacuate_region(ShenandoahHeapRegion* from_region, ShenandoahHeapRegion* to_region) {
  EvacuateRegionObjectClosure evacuate_region(epoch, this, to_region);
  from_region->object_iterate(&evacuate_region);
}

class ParallelEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* _sh;
  GrowableArray<ShenandoahHeapRegion*> _collectionSet;
  GrowableArray<ShenandoahHeapRegion*> _emptySet;
  GrowableArray<ShenandoahHeapRegion*> _finishedRegions;
  
public:  
  ParallelEvacuationTask(ShenandoahHeap* sh, 
			 GrowableArray<ShenandoahHeapRegion*> collectionSet,
			 GrowableArray<ShenandoahHeapRegion*> emptySet,
			 GrowableArray<ShenandoahHeapRegion*> finishedRegions) :
    AbstractGangTask("Parallel Evacuation Task"), 
    _sh(sh), 
    _collectionSet(collectionSet),
    _emptySet(emptySet),
    _finishedRegions(finishedRegions){}
  
  void work(uint worker_id) {
  
    for (int i = 0; i < _collectionSet.length(); i++) {
      ShenandoahHeapRegion* from_hr = _collectionSet.at(i);
      if (from_hr->claim()) {
	if (ShenandoahGCVerbose) {
	  tty->print("Thread %d claimed Heap Region %d\n",
		     worker_id,
		     from_hr->regionNumber);
	}
	int j = 0;
	bool done = false;

	// We have an earlier assert that ensures we have enough empty regions.
	// This will go away once we have parallel evacuation of regions.
	while (!done && j < _emptySet.length()) {
	  ShenandoahHeapRegion* to_hr = _emptySet.at(j++);
	  if (to_hr->claim()) {
	    done = true;
	    _sh->evacuate_region(from_hr, to_hr);
	  }
	}
      }
    }
  }
};

void ShenandoahHeap::parallel_evacuate() {
  // We need to refactor this

  ShenandoahCollectionSetChooser chooser;
  chooser.initialize(firstRegion);

  GrowableArray<ShenandoahHeapRegion*> collectionSet = chooser.cs_regions();
  GrowableArray<ShenandoahHeapRegion*> emptySet = chooser.empty_regions();
  GrowableArray<ShenandoahHeapRegion*> finishedRegions;

  ParallelEvacuationTask* evacuationTask = 
    new ParallelEvacuationTask(this, collectionSet, emptySet, finishedRegions);

  assert(emptySet.length() >= collectionSet.length(), 
	 "Collection set chooser should have provided the right number of empty regions");
  workers()->run_task(evacuationTask);
  
  if (! ShenandoahUseNewUpdateRefs) {
    update_references_after_evacuation();
    for (int i = 0; i < collectionSet.length(); i++) {
      ShenandoahHeapRegion* current = collectionSet.at(i);
      verify_evacuation(current);
      current->recycle();
    }
  }
  
}

void ShenandoahHeap::evacuate() {

  ShenandoahCollectionSetChooser chooser;
  chooser.initialize(firstRegion);

  GrowableArray<ShenandoahHeapRegion*> collectionSet = chooser.cs_regions();
  GrowableArray<ShenandoahHeapRegion*> emptySet = chooser.empty_regions();
  GrowableArray<ShenandoahHeapRegion*> finishedRegions;

  while (collectionSet.length() > 0) {
    ShenandoahHeapRegion* fromRegion = collectionSet.pop();
    ShenandoahHeapRegion* toRegion = emptySet.pop();
    //    tty->print("From Region:\n");
    //    fromRegion->print();
    //    tty->print("To Region:\n");
    //    toRegion->print();

    evacuate_region(fromRegion, toRegion);
    finishedRegions.append(fromRegion);
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
    oop heap_oop = *p;
    if (! oopDesc::is_null(heap_oop)) {
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: %p", heap_oop));
    }
  }

  void do_oop(narrowOop* p) {
    _heap->nyi();
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

oop ShenandoahHeap::get_brooks_ptr_oop_for(oop p) {
  HeapWord* oopWord = (HeapWord*) p;
  HeapWord* brooksPOop = oopWord - BROOKS_POINTER_OBJ_SIZE;
  assert(is_brooks_ptr(oop(brooksPOop)), "brooks pointer must be a brooks pointer");
  HeapWord** brooksP = (HeapWord**) (brooksPOop + BROOKS_POINTER_OBJ_SIZE - 1);
  HeapWord* forwarded = *brooksP;
  return (oop) forwarded;
}

void ShenandoahHeap::maybe_update_oop_ref(oop* p) {
  oop heap_oop = *p;
  if (! oopDesc::is_null(heap_oop)) {
    oop forwarded_oop = get_brooks_ptr_oop_for(heap_oop);
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
    _heap->nyi();
  }

};

void ShenandoahHeap::update_references_after_evacuation() {

  UpdateRefsAfterEvacuationClosure rootsCl;
  CodeBlobToOopClosure blobsCl(&rootsCl, false);
  KlassToOopClosure klassCl(&rootsCl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  ClassLoaderDataGraph::clear_claimed_marks();

  process_strong_roots(true, false, ScanningOption(so), &rootsCl, &blobsCl, &klassCl);

  oop_iterate(&rootsCl);
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
  nyi();
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  nyi();
  return NULL;
  
}

ShenandoahCollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _pgc_policy;
}


HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  nyi();
  return 0;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  nyi();
  return 0;
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  nyi();
  return false;
}

jlong ShenandoahHeap::millis_since_last_gc() {
  nyi();
  return 0;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  nyi();
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

  template <class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
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

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

class ShenandoahVerifyHeapClosure: public ObjectClosure {
private:
  ShenandoahVerifyRootsClosure _rootsCl;
  ShenandoahHeap* _heap;
public:
  ShenandoahVerifyHeapClosure(ShenandoahVerifyRootsClosure rc) :
    _rootsCl(rc), _heap(ShenandoahHeap::heap()) {};

  void do_object(oop p) {
    _rootsCl.do_oop(&p);

    HeapWord* oopWord = (HeapWord*) p;
    if (_heap->is_brooks_ptr(oop(oopWord))) { // Brooks pointer
      guarantee(arrayOop(oopWord)->length() == 1, "brooks ptr objects must have length == 1");
    } else {
      HeapWord* brooksPOop = (oopWord - BROOKS_POINTER_OBJ_SIZE);
      guarantee(_heap->is_brooks_ptr(oop(brooksPOop)), "age in mark word of brooks obj must be 15");
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
  nyi();
}

class ShenandoahIterateOopClosureRegionClosure : public ShenandoahHeapRegionClosure {
  MemRegion _mr;
  ExtendedOopClosure* _cl;
public:
  ShenandoahIterateOopClosureRegionClosure(ExtendedOopClosure* cl) : _cl(cl) {}
  ShenandoahIterateOopClosureRegionClosure(MemRegion mr, ExtendedOopClosure* cl) 
    :_mr(mr), _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->oop_iterate(_cl);
    return false;
  }
};

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl) {
  ShenandoahIterateOopClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void ShenandoahHeap::oop_iterate(MemRegion mr, 
				 ExtendedOopClosure* cl) {
  ShenandoahIterateOopClosureRegionClosure blk(mr, cl);
  heap_region_iterate(&blk);
}

void  ShenandoahHeap::object_iterate_since_last_GC(ObjectClosure* cl) {
  nyi();
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

// We need a data structure that maps addresses to heap regions.
// This will probably be the first thing to optimize.

class ContainsRegionClosure: public ShenandoahHeapRegionClosure {
  HeapWord* addr;
  ShenandoahHeapRegion* result;

public:
  ContainsRegionClosure(HeapWord* hw) :addr(hw) {}
    
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_in(addr)) {
      result = r;
      return true;
    }
    return false;
  }
};

template<class T>
inline ShenandoahHeapRegion*
ShenandoahHeap::heap_region_containing(const T addr) const {
  ContainsRegionClosure blk((HeapWord*) addr);
  heap_region_iterate(&blk);
}


Space*  ShenandoahHeap::space_containing(const void* oop) const {
  Space* res = heap_region_containing(oop);
  return res;
}

void  ShenandoahHeap::gc_prologue(bool b) {
  nyi();
}

void  ShenandoahHeap::gc_epilogue(bool b) {
  nyi();
}

// Apply blk->doHeapRegion() on all committed regions in address order,
// terminating the iteration early if doHeapRegion() returns true.
void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk) const {
  ShenandoahHeapRegion* current  = firstRegion;
  while (current != NULL && !blk->doHeapRegion(current)) {
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
  epoch(e), _worker_id(worker_id), _heap(ShenandoahHeap::heap()) {
}

void ShenandoahMarkRefsClosure::do_oop_work(oop* p) {
  oop obj = *p;
  if (ShenandoahUseNewUpdateRefs) {
    _heap->maybe_update_oop_ref(p);
  }
  ShenandoahMarkObjsClosure cl(epoch, _worker_id);
  cl.do_object(obj);
}

void ShenandoahMarkRefsClosure::do_oop(narrowOop* p) {
  assert(false, "narrorOops not supported");
}

void ShenandoahMarkRefsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

ShenandoahMarkObjsClosure::ShenandoahMarkObjsClosure(uint e, uint worker_id) : epoch(e), _worker_id(worker_id) {
}

void ShenandoahMarkObjsClosure::do_object(oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap* ) Universe::heap();
  if (obj != NULL && (sh->is_in(obj))) {
    // tty->print_cr("probably marking root object %p, %d, %d", obj, getMark(obj)->age(), epoch);
    if (getMark(obj)->age() != epoch) {
      setMark(obj, getMark(obj)->set_age(epoch));
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

  /* We are going to add all the roots to taskqueue 0 for now.  Later on we should experiment with a better partioning. */
void ShenandoahHeap::prepare_unmarked_root_objs() {
  ShenandoahMarkRefsClosure rootsCl(epoch, 0);
  CodeBlobToOopClosure blobsCl(&rootsCl, false);
  KlassToOopClosure klassCl(&rootsCl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  ClassLoaderDataGraph::clear_claimed_marks();

  process_strong_roots(true, false, ScanningOption(so), &rootsCl, &blobsCl, &klassCl);
}

void ShenandoahHeap::start_concurrent_marking() {
  set_concurrent_mark_in_progress(true);
  prepare_unmarked_root_objs();
}

// this should really be a closure as should printHeapLocations
size_t ShenandoahHeap::calcLiveness(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  size_t result = 0;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    if (isMarkedCurrent(oop(cur))) {
      result = result + oop(cur)->size() * HeapWordSize + (BROOKS_POINTER_OBJ_SIZE * HeapWordSize);
    }
  }
  return result;
}


class CalcLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  CalcLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->setLiveData(sh->calcLiveness(r->bottom(), r->top()));
    r->clearClaim();
    if (r->next() == NULL) 
      return true;
    else return false;
  }
};


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
  oop_iterate(&verifyLive);
}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(concurrent_mark_in_progress(), "How else could we get here?");
  set_concurrent_mark_in_progress(false);
  
  CalcLivenessClosure clc(this);
  heap_region_iterate(&clc);
  
  PrintHeapRegionsClosure pc;
  heap_region_iterate(&pc);
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
  setMark(obj, getMark(obj)->set_age(epoch));

}


bool ShenandoahHeap::isMarkedPrev(oop obj) const {
  return getMark(obj)->age() == epoch -1;
}

bool ShenandoahHeap::isMarkedCurrent(oop obj) const {
  return getMark(obj)->age() == epoch;
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
	tty->print("Verifying liveness of reference obj "PTR_FORMAT"\n", obj);
	obj->print();
      }
      assert(sh->isMarkedCurrent(obj), "Referenced Objects should be marked");
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
	  tty->print("Verifying liveness of objects pointed to by "PTR_FORMAT"\n", obj);
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
  oop_iterate(&verifyLive);
}

#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"

#include "gc_implementation/shared/vmGCOperations.hpp"

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
  _concurrent_mark_in_progress(false) {
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
  epoch = 1;
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

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {

   if (currentRegion == NULL) {
     assert(false, "No GC implemented");
   }

   HeapWord* filler = currentRegion->allocate(BROOKS_POINTER_OBJ_SIZE);
   HeapWord* result = NULL;
   if (filler != NULL) {
     result = currentRegion->allocate(size);
     if (result != NULL) {
       CollectedHeap::fill_with_array(filler, BROOKS_POINTER_OBJ_SIZE, false);
       markOop mark = oop(filler)->mark();
       oop(filler)->set_mark(mark->set_age(15));
       assert(oop(filler)->mark()->age() == 15, "age must be 15 here");

       // Set the brooks pointer
       HeapWord* first = filler + (BROOKS_POINTER_OBJ_SIZE - 1);
       uintptr_t first_ptr = (uintptr_t) first;
       *(unsigned long*)(((unsigned long*)first_ptr)) = (unsigned long) result;
       //tty->print_cr("result, brooks obj, brooks ptr: %p, %p, %p", result, filler, first);
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
   currentRegion = currentRegion->next();
   if (currentRegion == NULL) {
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

  // This is just an arbitrary number for now.  CHF
  size_t targetStartMarking = capacity() / 8;

  if (used() > targetStartMarking && !concurrent_mark_in_progress()) {
    tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT" Target = "SIZE_FORMAT" doing initMark\n", capacity(), used(), targetStartMarking);
    mark();

    //    PrintHeapObjectsClosure printObjs;
    //    heap_region_iterate(&printObjs);
  }

  MutexLocker ml(Heap_lock);
  return mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
}

void ShenandoahHeap::mark() {
    VM_ShenandoahInitMark initMark;
    VMThread::execute(&initMark);
    
    concurrentMark()->markFromRoots();

    VM_ShenandoahFinishMark finishMark;
    VMThread::execute(&finishMark);

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
public:
  ShenandoahVerifyHeapClosure(ShenandoahVerifyRootsClosure rc) :
    _rootsCl(rc) {};

  void do_object(oop p) {
    _rootsCl.do_oop(&p);

    HeapWord* oopWord = (HeapWord*) p;
    if (oop(oopWord)->mark()->age() == 15) { // Brooks pointer
      // TODO: Implement brooks pointer checking.
    } else {
      HeapWord* brooksPOop = (oopWord - BROOKS_POINTER_OBJ_SIZE);
      guarantee(oop(brooksPOop)->mark()->age() == 15, "age in mark word of brooks obj must be 15");
      HeapWord** brooksP = (HeapWord**) (brooksPOop + (BROOKS_POINTER_OBJ_SIZE - 1));
      /*
      if (*brooksP == oopWord) {
	tty->print_cr("good oop: %p, %p, %p", oopWord, brooksP, *brooksP);
      } else {
	tty->print_cr("bad  oop: %p, %p, %p", oopWord, brooksP, *brooksP);
      }
      */
      guarantee(*brooksP == oopWord,
		err_msg("brooks pointer points to next oop: "PTR_FORMAT": "PTR_FORMAT"->"PTR_FORMAT,
			brooksPOop, *brooksP, oopWord));
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


ShenandoahMarkRefsClosure::ShenandoahMarkRefsClosure(uint e) : epoch(e) {
}

void ShenandoahMarkRefsClosure::do_oop_work(oop* p) {
  oop obj = *p;
  ShenandoahMarkObjsClosure cl(epoch);
  cl.do_object(obj);
}

void ShenandoahMarkRefsClosure::do_oop(narrowOop* p) {
  assert(false, "narrorOops not supported");
}

void ShenandoahMarkRefsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

ShenandoahMarkObjsClosure::ShenandoahMarkObjsClosure(uint e) : epoch(e) {
}

void ShenandoahMarkObjsClosure::do_object(oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap* ) Universe::heap();
  if (obj != NULL && (sh->is_in(obj))) {
    if (obj->has_displaced_mark()) {
      if (obj->displaced_mark()->age() != epoch) {
        obj->set_displaced_mark(obj->displaced_mark()->set_age(epoch));
        sh->concurrentMark()->addTask(obj);
      }
    } else {
      if (obj->mark()->age() != epoch) {
        obj->set_mark(obj->mark()->set_age(epoch));
        sh->concurrentMark()->addTask(obj);
      }
    }
  }
}

  /* We are going to add all the roots to taskqueue 0 for now.  Later on we should experiment with a better partioning. */
void ShenandoahHeap::prepare_unmarked_root_objs() {
  ShenandoahMarkRefsClosure rootsCl(epoch);
  CodeBlobToOopClosure blobsCl(&rootsCl, false);
  KlassToOopClosure klassCl(&rootsCl);

  const int so = SO_AllClasses | SO_Strings | SO_CodeCache;

  process_strong_roots(true, false, ScanningOption(so), &rootsCl, &blobsCl, &klassCl);
}

void ShenandoahHeap::start_concurrent_marking() {
  concurrentMark()->setEpoch(epoch);
  if (! concurrent_mark_in_progress()) {
    set_concurrent_mark_in_progress(true);

    prepare_unmarked_root_objs();
  }
}

// this should really be a closure as should printHeapLocations
size_t ShenandoahHeap::calcLiveness(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  size_t result = 0;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    if (isMarkedCurrent(oop(cur)))
      result = result + oop(cur)->size();
  }
  return result;
}


class CalcLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  CalcLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r-> setLiveData(sh->calcLiveness(r->bottom(), r->top()));
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
      assert(sh->isMarkedCurrent(obj), "Referenced Objects should be marked");
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
      if (sh->isMarkedCurrent(obj)) {
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

bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}

bool ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  _concurrent_mark_in_progress = in_progress;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, ! in_progress);
}



#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"
#include "memory/oopFactory.hpp"


ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

class PrintHeapRegionsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x\n", 
	       r->regionNumber, r->top(), r->used(), r->free());
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

  _sct = new ShenandoahConcurrentThread();
  if (_sct == NULL)
    return JNI_ENOMEM;
  
  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) : 
  SharedHeap(policy),
  _pgc_policy(policy), 
  _sct(),
  _pgc_barrierSet(new ShenandoahBarrierSet()) {
  _pgc = this;
  epoch = 1;
  set_barrier_set(_pgc_barrierSet);
  // Where does this really belong?
  oopDesc::set_bs(_pgc_barrierSet);
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
  ShenandoahConcurrentThread* first = new ShenandoahConcurrentThread();
}

size_t ShenandoahHeap::capacity() const {
  return initialSize;
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

bool ShenandoahHeap::is_maximal_no_gc() const {
  nyi();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return initialSize;
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
  int *val = NULL;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    oop(cur)->print();
    printHeapLocations(cur, cur + oop(cur)->size());
  }
}

ShenandoahHeap* ShenandoahHeap::heap() {
  assert(_pgc != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(_pgc->kind() == CollectedHeap::ShenandoahHeap, "not a shenandoah heap");
  return _pgc;
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
					      bool* gc_overhead_limit_was_exceeded) {

   if (currentRegion == NULL) {
     assert(false, "No GC implemented");
   }

   HeapWord* filler = currentRegion->allocate(4);
   HeapWord* result = NULL;
   if (filler != NULL) {
     CollectedHeap::fill_with_array(filler, 4, false);
     result = currentRegion->allocate(size);
     if (result != NULL) {
       // Set the brooks pointer
       HeapWord* first = filler+3;
       uintptr_t first_ptr = (uintptr_t) first;
       *(unsigned long*)(((unsigned long*)first_ptr)) = (unsigned long) result;
       return result;
     }
   }
   assert (result == NULL, "expect result==NULL");

   tty->print("Closing region %d "PTR_FORMAT"["PTR_FORMAT":%d] and starting region %d "PTR_FORMAT" total used in bytes = "SIZE_FORMAT" M \n", 
	      currentRegion->regionNumber, currentRegion, 
	      currentRegion->top(), currentRegion->used(),
	      currentRegion->next() != NULL ? currentRegion->next()->regionNumber : -1,
	      currentRegion->next(),
	      used_in_bytes()/M);

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


HeapWord*  ShenandoahHeap::mem_allocate(size_t size, 
					bool*  gc_overhead_limit_was_exceeded) {

  MutexLocker ml(Heap_lock);
  return mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
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

CollectorPolicy* ShenandoahHeap::collector_policy() const {
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

void ShenandoahHeap::verify(bool silent , VerifyOption vo) {
  // Needed to keep going
}

size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return 0;
}

class IterateObjectClosureRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosure* _cl;
public:
  IterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate(_cl);
    return false;
  }
};

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  nyi();
}

class IterateOopClosureRegionClosure : public ShenandoahHeapRegionClosure {
  MemRegion _mr;
  ExtendedOopClosure* _cl;
public:
  IterateOopClosureRegionClosure(ExtendedOopClosure* cl) : _cl(cl) {}
  IterateOopClosureRegionClosure(MemRegion mr, ExtendedOopClosure* cl) 
    :_mr(mr), _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->oop_iterate(_cl);
    return false;
  }
};

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl) {
  IterateOopClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

void ShenandoahHeap::oop_iterate(MemRegion mr, 
				 ExtendedOopClosure* cl) {
  IterateOopClosureRegionClosure blk(mr, cl);
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
  while (!blk->doHeapRegion(current)) {
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

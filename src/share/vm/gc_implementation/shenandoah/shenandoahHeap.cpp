#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"
#include "memory/oopFactory.hpp"


jint ShenandoahHeapRegion::initialize(HeapWord* start, 
				      size_t regionSize) {
  MemRegion reserved = MemRegion((HeapWord*) start,
		       (HeapWord*) start + regionSize);
  ContiguousSpace::initialize(reserved, true, false);
  return JNI_OK;
}

// // Private variables
// ShenandoahHeapRegion *ShenandoahHeap::firstRegion = NULL;
// ShenandoahHeapRegion *ShenandoahHeap::currentRegion = NULL;
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

  ReservedSpace heap_rs = Universe::reserve_heap(initialSize,
				 ShenandoahHeap::alignment);
  
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*) (heap_rs.base() + heap_rs.size()));

  ReservedSpace pgc_rs = heap_rs.first_part(initialSize);
  

  tty->print("Calling initialize on reserved space base = %p end = %p\n", 
	     pgc_rs.base(), pgc_rs.base() + pgc_rs.size());
  
  ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
  firstRegion = current;
  currentRegion = firstRegion;
  
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

  PrintHeapRegionsClosure pc;
  heap_region_iterate(&pc);

  

  // HeapWord* pointer = hr_rs.base();
  // size_t regions = 0;
  // ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
  // current.initialize(pointer+=region_size, region_size);
  // firstRegion = current;

  // for (regions=0; regions < numRegions; regions++) {
  //   ShenandoahHeapRegion* next = new ShenandoahHeapRegion();
  //   next->initialize(pointer+=region_size, region_size);
  //   current->set_next(next);
  //   current = next;
  // }

  return JNI_OK;
}


ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) : 
  SharedHeap(policy),
  _pgc_policy(policy), 
  _pgc_barrierSet(new ShenandoahBarrierSet()) {
  _pgc = this;
  set_barrier_set(_pgc_barrierSet);
  // Where does this really belong?
  oopDesc::set_bs(_pgc_barrierSet);
}


void ShenandoahHeap::nyi() const {
  assert(false, "not yet implemented");
  tty->print("not yet implmented\n");
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("ShenandoahHeap");
}

// void initializeRegions(ReservedSpace heap_rs, size_t numRegions, size_t region_size, size_t alignment) {
//   HeapWord* top = heap_rs.base();

//   ShenandoahHeapRegion* current = new ShenandoahHeapRegion(heap_rs, top, region_size);
//   top = top + region_size;

//   ShenandoahHeap::firstRegion = current;
//   size_t regions = 0;

//   for (regions = 0; regions < numRegions; regions++) {
//     ShenandoahHeapRegion* next = new ShenandoahHeapRegion(heap_rs, top, region_size);
//     current->regionNumber = regions;
//     current->initialize();
//     current->setNext(next);
//     current = next;
//     top = top + region_size;
//   }

//   current->regionNumber = regions;
//   current->initialize();
//   current->setNext(NULL);
// }

void ShenandoahHeap::post_initialize() {
  // Nothing needs to go here?
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

   tty->print("Closing region %d "PTR_FORMAT" and starting region %d "PTR_FORMAT"\n", 
	      currentRegion->regionNumber, currentRegion, 
	      currentRegion->next() != NULL ? currentRegion->next()->regionNumber : -1,
	      currentRegion->next());
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
  return regionSizeWords;
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
  return regionSizeWords;
}

void ShenandoahHeap::collect(GCCause::Cause) {
  nyi();
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  nyi();
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  nyi();
  return NULL;
  
}

CollectorPolicy* ShenandoahHeap::collector_policy() const {
  nyi();
  return NULL;
}

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl) {
  nyi();
}

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  nyi();
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  nyi();
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
  return regionSizeWords;
}

void ShenandoahHeap::oop_iterate(MemRegion mr, 
				 ExtendedOopClosure* ecl) {
  nyi();
}

void  ShenandoahHeap::object_iterate_since_last_GC(ObjectClosure* cl) {
  nyi();
}

void  ShenandoahHeap::space_iterate(SpaceClosure* scl) {
  nyi();
}

Space*  ShenandoahHeap::space_containing(const void* oop) const {
  nyi();
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

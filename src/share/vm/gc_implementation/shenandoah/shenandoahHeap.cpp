/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"

#include "gc_implementation/shared/cmBitMap.inline.hpp"
#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/referenceProcessor.hpp"
#include "memory/universe.hpp"
#include "utilities/copy.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "runtime/atomic.inline.hpp"

#define __ masm->

ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

void ShenandoahHeap::print_heap_locations(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur++) {
    tty->print_cr(PTR_FORMAT" : "PTR_FORMAT, p2i(cur), p2i(*((HeapWord**) cur)));
  }
}

void ShenandoahHeap::print_heap_objects(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    oop(cur)->print();
    print_heap_locations(cur, cur + oop(cur)->size());
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
    tty->print_cr("Region "INT32_FORMAT" top = "PTR_FORMAT" used = "SIZE_FORMAT_HEX" free = "SIZE_FORMAT_HEX, 
	       r->region_number(), p2i(r->top()), r->used(), r->free());
    
    ShenandoahHeap::heap()->print_heap_objects(r->bottom(), r->top());
    return false;
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();
  if (ShenandoahGCVerbose) 
    tty->print_cr("init_byte_size = "SIZE_FORMAT","SIZE_FORMAT_HEX"  max_byte_size = "INT64_FORMAT","SIZE_FORMAT_HEX, 
	     init_byte_size, init_byte_size, max_byte_size, max_byte_size);

  Universe::check_alignment(max_byte_size,  
			    ShenandoahHeapRegion::RegionSizeBytes, 
			    "shenandoah heap");
  Universe::check_alignment(init_byte_size, 
			    ShenandoahHeapRegion::RegionSizeBytes, 
			    "shenandoah heap");

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
						 Arguments::conservative_max_heap_alignment());
  initialize_reserved_region((HeapWord*)heap_rs.base(), (HeapWord*) (heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet());
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);
  _storage.initialize(pgc_rs, init_byte_size);
  if (ShenandoahGCVerbose) {
    tty->print_cr("Calling initialize on reserved space base = "PTR_FORMAT" end = "PTR_FORMAT, 
	       p2i(pgc_rs.base()), p2i(pgc_rs.base() + pgc_rs.size()));
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
    _free_regions->append(current);
    _ordered_regions[i] = current;
  }
  _first_region = _ordered_regions[0];
  _first_region_bottom = _first_region->bottom();
  assert((((size_t) _first_region_bottom) & (ShenandoahHeapRegion::RegionSizeBytes - 1)) == 0, err_msg("misaligned heap: "PTR_FORMAT, p2i(_first_region_bottom)));

  _numAllocs = 0;

  if (ShenandoahGCVerbose) {
    tty->print("All Regions\n");
    print_heap_regions();
    tty->print("Free Regions\n");
    _free_regions->print();
  }

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  // Reserve space for prev and next bitmap.
  size_t bitmap_size = CMBitMap::compute_size(heap_rs.size());
  MemRegion heap_region = MemRegion((HeapWord*) heap_rs.base(), heap_rs.size() / HeapWordSize);

  ReservedSpace bitmap_0(ReservedSpace::allocation_align_size_up(bitmap_size));
  os::commit_memory_or_exit(bitmap_0.base(), bitmap_0.size(), false, err_msg("couldn't allocate mark bitmap 0"));

  MemRegion bitmap_0_region = MemRegion((HeapWord*) bitmap_0.base(), bitmap_0.size() / HeapWordSize);
  _mark_bit_map0.initialize(heap_region, bitmap_0_region);

  ReservedSpace bitmap_1(ReservedSpace::allocation_align_size_up(bitmap_size));
  os::commit_memory_or_exit(bitmap_1.base(), bitmap_1.size(), false, err_msg("couldn't allocate mark bitmap 1"));
  MemRegion bitmap_1_region = MemRegion((HeapWord*) bitmap_1.base(), bitmap_1.size() / HeapWordSize);
  _mark_bit_map1.initialize(heap_region, bitmap_1_region);

  _prev_mark_bit_map = &_mark_bit_map0;
  _next_mark_bit_map = &_mark_bit_map1;
  reset_mark_bitmap();

  // Initialize fast collection set test structure.
  _in_cset_fast_test_length = _max_regions;
  _in_cset_fast_test_base =
                   NEW_C_HEAP_ARRAY(bool, (size_t) _in_cset_fast_test_length, mtGC);
  _in_cset_fast_test = _in_cset_fast_test_base -
               ((uintx) pgc_rs.base() >> ShenandoahHeapRegion::RegionSizeShift);
  clear_cset_fast_test();

  _concurrent_gc_thread = new ShenandoahConcurrentThread();
  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) : 
  SharedHeap(policy),
  _shenandoah_policy(policy), 
  _concurrent_mark_in_progress(false),
  _evacuation_in_progress(false),
  _update_references_in_progress(false),
  _free_regions(NULL),
  _collection_set(NULL),
  _bytesAllocSinceCM(0),
  _bytes_allocated_since_last_cm(0),
  _max_allocated_gc(0),
  _allocated_last_gc(0),
  _used_start_gc(0),
  _max_workers((int) MAX2((uint) ParallelGCThreads, 1U)),
  _max_conc_workers((int) MAX2((uint) ConcGCThreads, 1U)),
  _ref_processor_cm(NULL),
  _in_cset_fast_test(NULL),
  _in_cset_fast_test_base(NULL),
  _tracer(new (ResourceObj::C_HEAP, mtGC) ShenandoahTracer()),
  _mark_bit_map0(),
  _mark_bit_map1(),
  _cancelled_evacuation(false) {
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
  _used = 0;
  
}

void ShenandoahHeap::reset_mark_bitmap() {
  CMBitMap* current = _next_mark_bit_map;
  _next_mark_bit_map = _prev_mark_bit_map;
  _prev_mark_bit_map = current;
  _next_mark_bit_map->clearAll();
}

class BitmapClearClosure : public BitMapClosure {
private:
  CMBitMap* _bm;

public:

  BitmapClearClosure(CMBitMap* bm) : _bm(bm) {
  }

  bool do_bit(BitMap::idx_t offset) {
    HeapWord* hw = _bm->offsetToHeapWord(offset);
    bool is_marked = _bm->isMarked(hw);
    return ! is_marked;
  }
};

bool ShenandoahHeap::is_bitmap_clear() {
  
  BitmapClearClosure bitmap_clear_cl(_next_mark_bit_map);
  return _next_mark_bit_map->iterate(&bitmap_clear_cl);
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("Shenandoah Heap");
  st->print(" total = " SIZE_FORMAT " K, used " SIZE_FORMAT " K ", capacity()/ K, used() /K);
  st->print("Region size = " SIZE_FORMAT "K \n", ShenandoahHeapRegion::RegionSizeBytes / K);
  if (Verbose) {
    print_heap_regions(st);
  }
}

class InitGCLABClosure : public ThreadClosure {
  void do_thread(Thread* thread) {
    thread->tlab().initialize();
  }
};

void ShenandoahHeap::post_initialize() {

  InitGCLABClosure init_gclabs;
  gc_threads_do(&init_gclabs);

  _scm->initialize();
  ref_processing_init();
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
  _used += bytes;
  // Atomic::add_ptr(bytes, &_used);
}

void ShenandoahHeap::set_used(size_t bytes) {
  _used = bytes;
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(_used - bytes >= 0, "never decrease heap size by more than we've left");
  _used -= bytes;
  // Atomic::add_ptr(-bytes, &_used);
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
  HeapWord* result = allocate_memory(word_size, false);

  if (result != NULL) {
    if (_concurrent_mark_in_progress || (ShenandoahUpdateRefsEarly && _evacuation_in_progress)) {
      // We mark the whole tlab here, this way we avoid marking every single
      // allocated object.
      _next_mark_bit_map->parMarkRange(MemRegion(result, word_size));
    }
    assert(! heap_region_containing(result)->is_in_collection_set(), "Never allocate in dirty region");
    _bytesAllocSinceCM += word_size * HeapWordSize;

#ifdef ASSERT
    if (ShenandoahTraceTLabs)
      tty->print_cr("allocating new tlab of size "SIZE_FORMAT" at addr "PTR_FORMAT, word_size, p2i(result));
#endif

  }
  return result;
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
  HeapWord* result = allocate_memory(word_size, true);
  if (result == NULL) {
    oom_during_evacuation();
    return NULL;
  }
  assert(! _concurrent_mark_in_progress, "no new gclabs during marking");
  assert(_evacuation_in_progress, "new gclabs only during evacuation");
  if (ShenandoahUpdateRefsEarly) {
    // We mark the whole tlab here, this way we avoid marking every single
    // allocated object.
    _next_mark_bit_map->parMarkRange(MemRegion(result, word_size));
  }
  assert(! heap_region_containing(result)->is_in_collection_set(), "Never allocate in dirty region");
  if (result != NULL) {
    if (ShenandoahTraceTLabs) {
      tty->print_cr("allocating new gclab of size "SIZE_FORMAT" at addr "PTR_FORMAT, word_size, p2i(result));
    }

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
     Universe::heap()->ensure_parsability(false);
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

HeapWord* ShenandoahHeap::allocate_memory(size_t word_size, bool evacuation) {
  HeapWord* result = NULL;
  result = allocate_memory_with_lock(word_size, evacuation);

  if (result == NULL && ! evacuation) { // Allocation failed, try full-GC, then retry allocation.
    collect(GCCause::_allocation_failure);
    result = allocate_memory_with_lock(word_size, evacuation);
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_with_lock(size_t word_size, bool evacuation) {
  if (Thread::current()->is_GC_task_thread() && SafepointSynchronize::is_at_safepoint()) {
    return allocate_memory_shenandoah_lock(word_size, evacuation);
  } else {
    return allocate_memory_heap_lock(word_size, evacuation);
  }
}

HeapWord* ShenandoahHeap::allocate_memory_heap_lock(size_t word_size, bool evacuation) {
  MutexLocker ml(Heap_lock);
  return allocate_memory_work(word_size, evacuation);
}

HeapWord* ShenandoahHeap::allocate_memory_shenandoah_lock(size_t word_size, bool evacuation) {
  MutexLocker ml(ShenandoahHeap_lock);
  return allocate_memory_work(word_size, evacuation);
}

ShenandoahHeapRegion* ShenandoahHeap::check_skip_humonguous(ShenandoahHeapRegion* region, bool evacuation) {
  while (region != NULL && region->is_humonguous()) {
    region = _free_regions->get_next(evacuation);
  }
  return region;
}

ShenandoahHeapRegion* ShenandoahHeap::get_next_region_skip_humonguous(bool evacuation) {
  ShenandoahHeapRegion* next = _free_regions->get_next(evacuation);
  return check_skip_humonguous(next, evacuation);
}

ShenandoahHeapRegion* ShenandoahHeap::get_current_region_skip_humonguous(bool evacuation) {
  ShenandoahHeapRegion* current = _free_regions->current(evacuation);
  return check_skip_humonguous(current, evacuation);
}


ShenandoahHeapRegion* ShenandoahHeap::check_grow_heap(ShenandoahHeapRegion* current, bool evacuation) {
  if (current == NULL) {
    if (grow_heap_by()) {
      current = _free_regions->get_next(evacuation);
      assert(current != NULL, "After successfully growing the heap we should have a region");
      assert(! current->is_humonguous(), "new region must not be humonguous");
    } else {
      current = NULL; // No more room to make a new region. OOM.
    }
  }
  return current;
}

ShenandoahHeapRegion* ShenandoahHeap::get_current_region(bool evacuation) {
  ShenandoahHeapRegion* current = get_current_region_skip_humonguous(evacuation);
  return check_grow_heap(current, evacuation);
}

ShenandoahHeapRegion* ShenandoahHeap::get_next_region(bool evacuation) {
  ShenandoahHeapRegion* current = get_next_region_skip_humonguous(evacuation);
  return check_grow_heap(current, evacuation);
}


HeapWord* ShenandoahHeap::allocate_memory_work(size_t word_size, bool evacuation) {

  if (word_size * HeapWordSize > ShenandoahHeapRegion::RegionSizeBytes) {
    assert(! evacuation, "no evacuation of humonguous objects");
    return allocate_large_memory(word_size);
  }

  ShenandoahHeapRegion* my_current_region = get_current_region(false);
  if (my_current_region == NULL) {
    return NULL; // No more room to make a new region. OOM.
  }
  assert(my_current_region != NULL, "should have a region at this point");

#ifdef ASSERT
  if (my_current_region->is_in_collection_set()) {
    print_heap_regions();
  }
#endif
  assert(! my_current_region->is_in_collection_set(), "never get targetted regions in free-lists");
  assert(! my_current_region->is_humonguous(), "never attempt to allocate from humonguous object regions");

  HeapWord* result;

  result = my_current_region->par_allocate(word_size);
  while (result == NULL && my_current_region != NULL) {
    // 2nd attempt. Try next region.
    my_current_region = get_next_region(false);
    if (my_current_region == NULL) {
      return NULL; // No more room to make a new region. OOM.
    }
    assert(my_current_region != NULL, "should have a region at this point");
    assert(! my_current_region->is_in_collection_set(), "never get targetted regions in free-lists");
    assert(! my_current_region->is_humonguous(), "never attempt to allocate from humonguous object regions");
    result = my_current_region->par_allocate(word_size);
  }

  if (result != NULL) {
    my_current_region->increase_live_data(word_size * HeapWordSize);
    increase_used(word_size * HeapWordSize);
  }
  return result;
}

HeapWord* ShenandoahHeap::allocate_large_memory(size_t words) {
  if (ShenandoahTraceHumonguous) {
    gclog_or_tty->print_cr("allocating humonguous object of size: "SIZE_FORMAT" words", words);
  }

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
    size_t live = words * HeapWordSize;
    free_regions[0]->increase_live_data(live);
    for (uint i = 0; i < required_regions; i++) {
      if (i == 0) {
        free_regions[0]->set_humonguous_start(true);
      } else {
        free_regions[i]->set_humonguous_continuation(true);
      }
      free_regions[i]->set_top(free_regions[i]->end());
    }
    result = free_regions[0]->bottom();
    increase_used(words * HeapWordSize);
  }
  return result;
}

bool ShenandoahHeap::find_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions) {
  if (ShenandoahTraceHumonguous) {
    gclog_or_tty->print_cr("trying to find "UINT32_FORMAT" contiguous free regions", num_free_regions);
  }
  uint free_regions_index = 0;
  for (uint regions_index = 0; regions_index < _num_regions; regions_index++) {
    // Claim a free region.
    ShenandoahHeapRegion* region = _ordered_regions[regions_index];
    bool free = false;
    if (region != NULL) {
      if (region->free() == ShenandoahHeapRegion::RegionSizeBytes) {
        assert(! region->is_humonguous(), "don't reuse occupied humonguous regions");
        free = true;
      }
    }
    if (! free) {
      // Not contiguous, reset search
      free_regions_index = 0;
      continue;
    }
    assert(free_regions_index >= 0 && free_regions_index < num_free_regions, "array bounds");
    free_regions[free_regions_index] = region;
    free_regions_index++;

    if (free_regions_index == num_free_regions) {
      if (ShenandoahTraceHumonguous) {
        gclog_or_tty->print_cr("found "UINT32_FORMAT" contiguous free regions:", num_free_regions);
        for (uint i = 0; i < num_free_regions; i++) {
          gclog_or_tty->print(UINT32_FORMAT": " , i);
          free_regions[i]->print(gclog_or_tty);
        }
      }
      return true;
    }

  }
  if (ShenandoahTraceHumonguous) {
    gclog_or_tty->print_cr("failed to find "UINT32_FORMAT" free regions", num_free_regions);
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

  HeapWord* filler = allocate_memory(BrooksPointer::BROOKS_POINTER_OBJ_SIZE + size, false);
  HeapWord* result = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  if (filler != NULL) {
    initialize_brooks_ptr(filler, result);
    _bytesAllocSinceCM += size * HeapWordSize;
#ifdef ASSERT
    if (ShenandoahTraceAllocations) {
      if (*gc_overhead_limit_was_exceeded)
	tty->print("gc_overhead_limit_was_exceeded");
      tty->print_cr("mem_allocate_locked object of size "SIZE_FORMAT" uat addr "PTR_FORMAT, size, p2i(result));
    }
#endif

    assert(! heap_region_containing(result)->is_in_collection_set(), "never allocate in targetted region");
    if (_concurrent_mark_in_progress || (ShenandoahUpdateRefsEarly && _evacuation_in_progress)) {
      mark_current_no_checks(oop(result));
    }

    return result;
  } else {
    tty->print_cr("Out of memory. Requested number of words: "SIZE_FORMAT" used heap: "INT64_FORMAT", bytes allocated since last CM: "INT64_FORMAT, size, used(), _bytesAllocSinceCM);
    {
      MutexLocker ml(Heap_lock);
      print_heap_regions();
      tty->print("Printing "SIZE_FORMAT" free regions:\n", _free_regions->length());
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
    tty->print_cr("References oop "PTR_FORMAT, p2i((HeapWord*) obj));
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
  Thread* _thread;
  public:
  ParallelEvacuateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) { 
  }

  void do_object(oop p) {

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print_cr("Calling ParallelEvacuateRegionObjectClosure on "PTR_FORMAT, p2i((HeapWord*) p));
    }
#endif

    if (_heap->isMarkedCurrent(p)) {
      _heap->evacuate_object(p, _thread);
    }
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
  from_region->object_iterate(&verify_evacuation, false);
}

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region) {

  if (from_region->getLiveData() == 0) {
    return;
  }

  ParallelEvacuateRegionObjectClosure evacuate_region(this);
  
#ifdef ASSERT
  if (ShenandoahGCVerbose) {
    tty->print_cr("parallel_evacuate_region starting from_region "INT32_FORMAT": free_regions = "SIZE_FORMAT,  from_region->region_number(), _free_regions->available_regions());
  }
#endif

  from_region->object_iterate(&evacuate_region, true);
#ifdef ASSERT
  if (ShenandoahVerify && ! cancelled_evacuation()) {
    verify_evacuated_region(from_region);
  }
  if (ShenandoahGCVerbose) {
    tty->print_cr("parallel_evacuate_region after from_region = "INT32_FORMAT": free_regions = "SIZE_FORMAT, from_region->region_number(), _free_regions->available_regions());
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

    while (from_hr != NULL) {
      if (ShenandoahGCVerbose) {
     	tty->print_cr("Thread "INT32_FORMAT" claimed Heap Region "INT32_FORMAT,
     		   worker_id,
     		   from_hr->region_number());
	from_hr->print();
      }

      // Not sure if the check is worth it or not.
      if (from_hr->getLiveData() != 0) {
	_sh->parallel_evacuate_region(from_hr);
      }
      if (_sh->cancelled_evacuation()) {
        break;
      }
      from_hr = _cs->claim_next();
    }

    if (ShenandoahGCVerbose) 
      tty->print("Thread "INT32_FORMAT" entering barrier sync\n", worker_id);

    _barrier_sync->enter();
    if (ShenandoahGCVerbose) 
      tty->print("Thread "INT32_FORMAT" post barrier sync\n", worker_id);

    Thread::current()->tlab().make_parsable(true);
  }
};

class RecycleDirtyRegionsClosure: public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeap* _heap;
public:
  RecycleDirtyRegionsClosure() : _heap(ShenandoahHeap::heap()) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {

    // If evacuation has been cancelled, we can't recycle regions, we only
    // clear their collection-set status.
    if (_heap->cancelled_evacuation()) {
      r->set_is_in_collection_set(false);
      return false;
    }

    if (r->is_in_collection_set()) {
      // tty->print_cr("recycling region "INT32_FORMAT":", r->region_number());
      // r->print_on(tty);
      // tty->print_cr("");
      _heap->decrease_used(r->used());
      r->recycle();
      if (ShenandoahUpdateRefsEarly) {
        _heap->free_regions()->append(r);
      }
    }

    return false;
  }
};

void ShenandoahHeap::recycle_dirty_regions() {
  RecycleDirtyRegionsClosure cl;
  heap_region_iterate(&cl);
  clear_cset_fast_test();
}

ShenandoahHeapRegionSet* ShenandoahHeap::free_regions() {
  return _free_regions;
}

void ShenandoahHeap::print_heap_regions(outputStream* st) const {
  PrintHeapRegionsClosure pc1(st);
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
	tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT")-> "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT")", _prefix, _index, p2i(p), p2i((HeapWord*) o), BOOL_TO_STR(ShenandoahHeap::heap()->isMarkedCurrent(o)), o->klass()->internal_name(), p2i(o->klass()));
      } else {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT" dirty: %s) -> "PTR_FORMAT" (not in heap, possibly corrupted or dirty (%s))", _prefix, _index, p2i(p), BOOL_TO_STR(ShenandoahHeap::heap()->heap_region_containing(p)->is_in_collection_set()), p2i((HeapWord*) o), BOOL_TO_STR(ShenandoahHeap::heap()->heap_region_containing(o)->is_in_collection_set()));
      }
    } else {
      tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT") -> "PTR_FORMAT, _prefix, _index, p2i(p), p2i((HeapWord*) o));
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
    tty->print_cr("%s object "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT") refers to:", _prefix, p2i((HeapWord*) p), BOOL_TO_STR(ShenandoahHeap::heap()->isMarkedCurrent(p)), p->klass()->internal_name(), p2i(p->klass()));
    PrintAllRefsOopClosure cl(_prefix);
    p->oop_iterate(&cl);
  }
};

void ShenandoahHeap::print_all_refs(const char* prefix) {
  tty->print_cr("printing all references in the heap");
  tty->print_cr("root references:");

  ensure_parsability(false);

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
	tty->print_cr("oop not marked, although referrer is marked: "PTR_FORMAT": in_heap: %s, is_marked: %s", 
		      p2i((HeapWord*) o), BOOL_TO_STR(_heap->is_in(o)), BOOL_TO_STR(_heap->isMarkedCurrent(o)));
	_heap->print_heap_locations((HeapWord*) o, (HeapWord*) o + o->size());

        tty->print_cr("oop class: %s", o->klass()->internal_name());
	if (_heap->is_in(p)) {
	  oop referrer = oop(_heap->heap_region_containing(p)->block_start_const(p));
	  tty->print_cr("Referrer starts at addr "PTR_FORMAT, p2i((HeapWord*) referrer));
	  referrer->print();
	  _heap->print_heap_locations((HeapWord*) referrer, (HeapWord*) referrer + referrer->size());
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
        tty->print_cr("oops has forwardee: p: "PTR_FORMAT" (%s), o = "PTR_FORMAT" (%s), new-o: "PTR_FORMAT" (%s)", p2i(p), BOOL_TO_STR(_heap->heap_region_containing(p)->is_in_collection_set()), p2i((HeapWord*) o),  BOOL_TO_STR(_heap->heap_region_containing(o)->is_in_collection_set()), p2i((HeapWord*) oopDesc::bs()->resolve_oop(o)), BOOL_TO_STR(_heap->heap_region_containing(oopDesc::bs()->resolve_oop(o))->is_in_collection_set()));
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
  ensure_parsability(false);
  VerifyAfterMarkingOopClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);
}

void ShenandoahHeap::prepare_for_concurrent_evacuation() {

  if (! ShenandoahUpdateRefsEarly) {
    recycle_dirty_regions();
  }

  ensure_parsability(true);

  // NOTE: This needs to be done during a stop the world pause, because
  // putting regions into the collection set concurrently with Java threads
  // will create a race. In particular, acmp could fail because when we
  // resolve the first operand, the containing region might not yet be in
  // the collection set, and thus return the original oop. When the 2nd
  // operand gets resolved, the region could be in the collection set
  // and the oop gets evacuated. If both operands have originally been
  // the same, we get false negatives.
  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  regions.reclaim_humonguous_regions();
  _collection_set->clear();
  _free_regions->clear();
  _shenandoah_policy->choose_collection_and_free_sets(&regions, _collection_set, _free_regions);


  _bytesAllocSinceCM = 0;

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

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  ShenandoahUpdateRootsClosure cl;
  CodeBlobToOopClosure blobsCl(&cl, false);
  CLDToOopClosure cldCl(&cl);

  ClassLoaderDataGraph::clear_claimed_marks();

  process_all_roots(true, SO_AllCodeCache, &cl, &cldCl, &blobsCl);
  ref_processor_cm()->weak_oops_do(&cl);
  process_weak_roots(&cl);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

class ShenandoahUpdateObjectsClosure : public ObjectClosureCareful {
  ShenandoahHeap* _heap;

public:
  ShenandoahUpdateObjectsClosure() :
    _heap(ShenandoahHeap::heap()) {
  }

  size_t do_object_careful(oop p) {
    ShenandoahUpdateRootsClosure refs_cl;
    assert(ShenandoahHeap::heap()->is_in(p), "only update objects in heap (where else?)");

    if (_heap->isMarkedCurrent(p)) {
      p->oop_iterate(&refs_cl);
    }
    return p->size();
  }

  size_t do_object_careful_m(oop p, MemRegion mr) {
    Unimplemented();
    return 0;
  }

  void do_object(oop p) {
    Unimplemented();
  }

};

class ParallelUpdateRefsTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;

public:
  ParallelUpdateRefsTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Update References Task"), 
  _regions(regions) {
  }

  void work(uint worker_id) {
    ShenandoahUpdateObjectsClosure update_refs_cl;
    ShenandoahHeapRegion* region = _regions->claim_next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    while (region != NULL) {
      if ((! region->is_in_collection_set() || heap->cancelled_evacuation()) && ! region->is_humonguous_continuation()) {
        HeapWord* failed = region->object_iterate_careful(&update_refs_cl);
        assert(failed == NULL, "careful iteration is implemented safe for now in Shenandaoh");
      }
      region = _regions->claim_next();
    }
  }
};

class RetireTLABClosure : public ThreadClosure {
private:
  bool _retire;

public:
  RetireTLABClosure(bool retire) : _retire(retire) {
  }

  void do_thread(Thread* thread) {
    thread->tlab().make_parsable(_retire);
  }
};

void ShenandoahHeap::ensure_parsability(bool retire_tlabs) {
  CollectedHeap::ensure_parsability(retire_tlabs);

  RetireTLABClosure cl(retire_tlabs);
  gc_threads_do(&cl);
}

void ShenandoahHeap::prepare_for_update_references() {
  ensure_parsability(true);

  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  regions.set_concurrent_iteration_safe_limits();

  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(false);

    // We need to update the roots so that they are ok for C2 when returning from the safepoint.
    update_roots();

    set_from_region_protection(true);

  } else {
    // We need to update the roots so that they are ok for C2 when returning from the safepoint.
    update_roots();
  }

  set_update_references_in_progress(true);
}

void ShenandoahHeap::update_references() {

  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  ParallelUpdateRefsTask task = ParallelUpdateRefsTask(&regions);
  workers()->set_active_workers(_max_conc_workers);
  workers()->run_task(&task);
  workers()->set_active_workers(_max_workers);

  VM_ShenandoahUpdateRootRefs update_roots;
  if (ShenandoahConcurrentUpdateRefs) {
    VMThread::execute(&update_roots);
  } else {
    update_roots.doit();
  }

  _allocated_last_gc = used() - _used_start_gc;
  size_t max_allocated_gc = MAX2(_max_allocated_gc, _allocated_last_gc);
  /*
  tty->print_cr("prev max_allocated_gc: "SIZE_FORMAT", new max_allocated_gc: "SIZE_FORMAT", allocated_last_gc: "SIZE_FORMAT" diff %f", _max_allocated_gc, max_allocated_gc, _allocated_last_gc, ((double) max_allocated_gc/ (double) _allocated_last_gc));
  */
  _max_allocated_gc = max_allocated_gc;
 
 set_update_references_in_progress(false);

 // In case evacuation has been cancelled, the cancallation protocol is done here.
 _cancelled_evacuation = false;
}


class ShenandoahEvacuateUpdateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
public:
  ShenandoahEvacuateUpdateRootsClosure() :
    _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
  }

  void do_oop(oop* p) {
    assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

    oop obj = oopDesc::load_heap_oop(p);
    if (obj != NULL && _heap->in_cset_fast_test((HeapWord*) obj)) {
      assert(_heap->is_marked_current(obj), "only evacuate marked objects");
      oopDesc::store_heap_oop(p, _heap->evacuate_object(obj, _thread));
    }
#ifdef ASSERT
    else if (! oopDesc::is_null(obj)) {
      // tty->print_cr("not updating root at: "PTR_FORMAT" with object: "PTR_FORMAT", is_in_heap: %s, is_in_cset: %s, is_marked: %s", p2i(p), p2i((HeapWord*) obj), BOOL_TO_STR(_heap->is_in(obj)), BOOL_TO_STR(_heap->in_cset_fast_test(obj)), BOOL_TO_STR(_heap->is_marked_current(obj)));
    }
#endif
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

class ShenandoahEvacuateUpdateStrongRootsTask : public AbstractGangTask {
public:

  ShenandoahEvacuateUpdateStrongRootsTask() : AbstractGangTask("Shenandoah evacuate and update strong roots") {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahEvacuateUpdateRootsClosure cl;
    CodeBlobToOopClosure blobsCl(&cl, false);
    CLDToOopClosure cldCl(&cl);

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ResourceMark rm;
    heap->process_all_roots(false, SharedHeap::SO_AllCodeCache, &cl, &cldCl, &blobsCl);
    heap->ref_processor_cm()->weak_oops_do(&cl);
    heap->process_weak_roots(&cl);

  }
};

class ShenandoahEvacuateUpdateWeakRootsTask : public AbstractGangTask {
public:

  ShenandoahEvacuateUpdateWeakRootsTask() : AbstractGangTask("Shenandoah evacuate and update weak roots") {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahEvacuateUpdateRootsClosure cl;
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    heap->weak_roots_iterate(&cl);

  }
};

void ShenandoahHeap::evacuate_and_update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());
  
  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(false);
  }

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");
  set_par_threads(_max_workers); // Prepare for parallel processing.
  ClassLoaderDataGraph::clear_claimed_marks();
  SharedHeap::StrongRootsScope strong_roots_scope(this, true);
  ShenandoahEvacuateUpdateStrongRootsTask strong_roots_task;
  workers()->run_task(&strong_roots_task);
  set_par_threads(0); // Prepare for serial processing in future calls to process_strong_roots.

  // We process weak roots using only 1 worker thread, multi-threaded weak roots
  // processing is not implemented yet. We can't use the VMThread itself, because
  // we need to grab the Heap_lock.
  ShenandoahEvacuateUpdateWeakRootsTask weak_roots_task;
  set_par_threads(1); // Prepare for parallel processing.
  workers()->set_active_workers(1);
  workers()->run_task(&weak_roots_task);
  workers()->set_active_workers(workers()->total_workers());
  set_par_threads(0); // Prepare for serial processing in future calls to process_strong_roots.

  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(true);
  }

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

    if (ShenandoahVerifyReadsToFromSpace) {
      set_from_region_protection(false);

      update_roots();

      set_from_region_protection(true);

    } else {
      update_roots();
    }
  }

  if (ShenandoahVerify && ! cancelled_evacuation()) {
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
    tty->print("Printing collection set which contains "SIZE_FORMAT" regions:\n", _collection_set->available_regions());
    _collection_set->print();
  }

  barrierSync.set_n_workers(_max_conc_workers);
  
  ParallelEvacuationTask evacuationTask = ParallelEvacuationTask(this, _collection_set, &barrierSync);

  workers()->set_active_workers(_max_conc_workers);
  workers()->run_task(&evacuationTask);
  workers()->set_active_workers(_max_workers);

  if (ShenandoahGCVerbose) {

    tty->print("Printing postgc collection set which contains "SIZE_FORMAT" regions:\n", _collection_set->available_regions());
    _collection_set->print();

    tty->print("Printing postgc free regions which contain "SIZE_FORMAT" free regions:\n", _free_regions->available_regions());
    _free_regions->print();

    tty->print_cr("finished parallel_evacuate");
    print_heap_regions();

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
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: "PTR_FORMAT, p2i((HeapWord*) heap_oop)));
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahHeap::roots_iterate(ExtendedOopClosure* cl) {

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  CodeBlobToOopClosure blobsCl(cl, false);
  CLDToOopClosure cldCl(cl);

  ClassLoaderDataGraph::clear_claimed_marks();

  process_all_roots(true, SO_AllCodeCache, cl, &cldCl, &blobsCl);
}

void ShenandoahHeap::weak_roots_iterate(ExtendedOopClosure* cl) {
  ref_processor_cm()->weak_oops_do(cl);
  process_weak_roots(cl);
}

void ShenandoahHeap::verify_evacuation(ShenandoahHeapRegion* from_region) {

  VerifyEvacuationClosure rootsCl(from_region);
  roots_iterate(&rootsCl);

}

oop ShenandoahHeap::maybe_update_oop_ref(oop* p) {

  assert((! is_in(p)) || (! heap_region_containing(p)->is_in_collection_set()) || cancelled_evacuation(),
         "never update refs in from-space, unless evacuation has been cancelled"); 

  oop heap_oop = *p; // read p
  if (! oopDesc::is_null(heap_oop)) {

#ifdef ASSERT
    if (! is_in(heap_oop)) {
      print_heap_regions();
      tty->print_cr("object not in heap: "PTR_FORMAT", referenced by: "PTR_FORMAT, p2i((HeapWord*) heap_oop), p2i(p));
      assert(is_in(heap_oop), "object must be in heap");
    }
#endif
    assert(is_in(heap_oop), "only ever call this on objects in the heap");
    assert((! (is_in(p) && heap_region_containing(p)->is_in_collection_set())) || cancelled_evacuation(), "we don't want to update references in from-space");
    oop forwarded_oop = ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop); // read brooks ptr
    if (forwarded_oop != heap_oop) {
      // tty->print_cr("updating old ref: "PTR_FORMAT" pointing to "PTR_FORMAT" to new ref: "PTR_FORMAT, p2i(p), p2i(heap_oop), p2i(forwarded_oop));
      assert(forwarded_oop->is_oop(), "oop required");
      assert(is_in(forwarded_oop), "forwardee must be in heap");
      assert(! heap_region_containing(forwarded_oop)->is_in_collection_set(), "forwardee must not be in collection set");
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
      tty->print_cr("not updating ref: "PTR_FORMAT, p2i(heap_oop));
      }
    */
  }
  return NULL;
}

bool ShenandoahHeap::supports_tlab_allocation() const {
  return true;
}


size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  return ShenandoahHeapRegion::RegionSizeBytes;
}

class ResizeGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->tlab().resize();
  }
};

void ShenandoahHeap::resize_all_tlabs() {
  CollectedHeap::resize_all_tlabs();

  if (PrintTLAB && Verbose) {
    tty->print_cr("Resizing Shenandoah GCLABs...");
  }

  ResizeGCLABClosure cl;
  gc_threads_do(&cl);

  if (PrintTLAB && Verbose) {
    tty->print_cr("Done resizing Shenandoah GCLABs...");
  }
}

class AccumulateStatisticsGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->tlab().accumulate_statistics();
    thread->tlab().initialize_statistics();
  }
};

void ShenandoahHeap::accumulate_statistics_all_gclabs() {

  AccumulateStatisticsGCLABClosure cl;
  gc_threads_do(&cl);
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
  if (GCCause::is_user_requested_gc(cause)) {
    if (! DisableExplicitGC) {
      if (ShenandoahTraceFullGC) {
        gclog_or_tty->print_cr("Shenandoah-full-gc: requested full GC");
      }
      _concurrent_gc_thread->do_full_gc();
    }
  } else if (cause == GCCause::_allocation_failure) {

    if (ShenandoahTraceFullGC) {
      gclog_or_tty->print_cr("Shenandoah-full-gc: full GC for allocation failure");
    }
    collector_policy()->set_should_clear_all_soft_refs(true);
      _concurrent_gc_thread->do_full_gc();

  } else if (cause == GCCause::_gc_locker) {

    if (ShenandoahTraceJNICritical) {
      gclog_or_tty->print_cr("Resuming deferred evacuation after JNI critical regions");
    }

    _concurrent_gc_thread->notify_jni_critical();
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
    if (is_evacuation_in_progress()) {
      cancel_evacuation();
    }
    ensure_parsability(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
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
	  gclog_or_tty->print_cr("Root location "PTR_FORMAT
				 "verified "PTR_FORMAT, p2i(p), p2i((void*) obj));
	  //	  obj->print_on(gclog_or_tty);
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
      gclog_or_tty->print("verify failures: %s", BOOL_TO_STR(failures));

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
  // We have all the heap available for tlabs.
  return capacity();
}

class ShenandoahIterateObjectClosureRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosure* _cl;
public:
  ShenandoahIterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate(_cl, false);
    return false;
  }
};

class ShenandoahIterateObjectClosureCarefulRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosureCareful* _cl;
public:
  ShenandoahIterateObjectClosureCarefulRegionClosure(ObjectClosureCareful* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate_careful(_cl);
    return false;
  }
};

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  ShenandoahIterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk, false, true);
}

void ShenandoahHeap::object_iterate_careful(ObjectClosureCareful* cl) {
  ShenandoahIterateObjectClosureCarefulRegionClosure blk(cl);
  heap_region_iterate(&blk, false, true);
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
  heap_region_iterate(&blk, skip_dirty_regions, true);
}

void ShenandoahHeap::oop_iterate(MemRegion mr, 
				 ExtendedOopClosure* cl) {
  ShenandoahIterateOopClosureRegionClosure blk(mr, cl);
  heap_region_iterate(&blk, false, true);
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

uint ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr); // & ~(ShenandoahHeapRegion::RegionSizeBytes - 1);
  uintptr_t index = (region_start - (uintptr_t) _first_region_bottom) >> ShenandoahHeapRegion::RegionSizeShift;
#ifdef ASSERT
  if (!(index >= 0 && index < _num_regions)) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT", region_size: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions[0]->bottom()), _num_regions, ShenandoahHeapRegion::RegionSizeBytes);
  }
#endif
  assert(index >= 0 && index < _num_regions, "heap region index must be in range");
  return index;
}

ShenandoahHeapRegion*
ShenandoahHeap::heap_region_containing(const void* addr) const {
  uint index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* result = _ordered_regions[index];
#ifdef ASSERT
  if (!(addr >= result->bottom() && addr < result->end())) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions[0]->bottom()), _num_regions);
  }
#endif
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
void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions, bool skip_humonguous_continuation) const {
  for (size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* current  = _ordered_regions[i];
    if (skip_humonguous_continuation && current->is_humonguous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && current->is_in_collection_set()) {
      continue;
    }
    if (blk->doHeapRegion(current)) { 
      return;
    }
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

class ClearLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  ClearLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }
  
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->clearLiveData();
    return false;
  }
};


void ShenandoahHeap::start_concurrent_marking() {

  accumulate_statistics_all_tlabs();

  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  if (UseTLAB) {
    ensure_parsability(true);
  }

  _shenandoah_policy->record_bytes_allocated(_bytesAllocSinceCM);
  _used_start_gc = used();

#ifdef ASSERT
  if (ShenandoahDumpHeapBeforeConcurrentMark) {
    ensure_parsability(false);
    print_all_refs("pre-mark");
  }
#endif
  
  ClearLivenessClosure clc(this);
  heap_region_iterate(&clc);

  // print_all_refs("pre -mark");

  // oopDesc::_debug = true;

  concurrentMark()->prepare_unmarked_root_objs();

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
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %s, is_forwarded: %s",
                        BOOL_TO_STR(_sh->heap_region_containing(obj)->is_in_collection_set()),
                        BOOL_TO_STR(obj != oopDesc::bs()->resolve_oop(obj)))
                );
      obj = oopDesc::bs()->resolve_oop(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_in_collection_set(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (! sh->isMarkedCurrent(obj)) {
        sh->print_on(tty);
      }
      assert(sh->isMarkedCurrent(obj), err_msg("Referenced Objects should be marked obj: "PTR_FORMAT", marked: %s, is_in_heap: %s", 
                                               p2i((HeapWord*) obj), BOOL_TO_STR(sh->isMarkedCurrent(obj)), BOOL_TO_STR(sh->is_in(obj))));
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
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %s, is_forwarded: %s obj-klass: %s, marked: %s",
                        BOOL_TO_STR(_sh->heap_region_containing(obj)->is_in_collection_set()),
                        BOOL_TO_STR(obj != oopDesc::bs()->resolve_oop(obj)), obj->klass()->external_name(), BOOL_TO_STR(_sh->is_marked_current(obj)))
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

class VerifyAfterUpdateRefsClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyAfterUpdateRefsClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee((! _sh->heap_region_containing(obj)->is_in_collection_set()) || _sh->cancelled_evacuation(),
                err_msg("no live reference must point to from-space, is_marked: %s",
                        BOOL_TO_STR(_sh->is_marked_current(obj))));
      if (obj != oopDesc::bs()->resolve_oop(obj) && _sh->is_in(p)) {
        tty->print_cr("top-limit: "PTR_FORMAT", p: "PTR_FORMAT, p2i(_sh->heap_region_containing(p)->concurrent_iteration_safe_limit()), p2i(p));
      }
      guarantee(obj == oopDesc::bs()->resolve_oop(obj), "no live reference must point to forwarded object");
      guarantee(obj->is_oop(), "is_oop");
      guarantee(Metaspace::contains(obj->klass()), "klass pointer must go to metaspace");
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_heap_after_evacuation() {

  ensure_parsability(false);

  VerifyAfterEvacuationClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);

}

class VerifyRegionsAfterUpdateRefsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    assert(! r->is_in_collection_set(), "no region must be in collection set");
    assert(! ShenandoahHeap::heap()->in_cset_fast_test(r->bottom()), "no region must be in collection set");
    return false;
  }
};

void ShenandoahHeap::verify_regions_after_update_refs() {
  VerifyRegionsAfterUpdateRefsClosure verify_regions;
  heap_region_iterate(&verify_regions);
}

void ShenandoahHeap::verify_heap_after_update_refs() {

  ensure_parsability(false);

  VerifyAfterUpdateRefsClosure cl;

  roots_iterate(&cl);
  weak_roots_iterate(&cl);
  oop_iterate(&cl, true, true);

}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(concurrent_mark_in_progress(), "How else could we get here?");
  set_concurrent_mark_in_progress(false);

  if (ShenandoahGCVerbose) {
    print_heap_regions();
  }

#ifdef ASSERT
  if (ShenandoahVerify) {
    verify_heap_after_marking();
  }

#endif
}

bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  if (ShenandoahTracePhases) {
    if (in_progress) {
      gclog_or_tty->print_cr("Shenandoah starting concurrent marking");
    } else {
      gclog_or_tty->print_cr("Shenandoah finishing concurrent marking");
    }
  }

  _concurrent_mark_in_progress = in_progress;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, ! in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  if (ShenandoahTracePhases) {
    if (ShenandoahConcurrentEvacuation) {
      if (in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting concurrent evacuation");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing concurrent evacuation");
      }
    } else {
      if (in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting non-concurrent evacuation");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing non-concurrent evacuation");
      }
    }
  }
  _evacuation_in_progress = in_progress;
}

bool ShenandoahHeap::is_evacuation_in_progress() {
  return _evacuation_in_progress;
}

bool ShenandoahHeap::is_update_references_in_progress() {
  return _update_references_in_progress;
}

void ShenandoahHeap::set_update_references_in_progress(bool update_refs_in_progress) {
  if (ShenandoahTracePhases && ShenandoahUpdateRefsEarly) {
    if (ShenandoahConcurrentUpdateRefs) {
      if (update_refs_in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting concurrent reference-updating");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing concurrent reference-updating");
      }
    } else {
      if (update_refs_in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting non-concurrent reference-updating");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing non-concurrent reference-updating");
      }
    }
  }
  _update_references_in_progress = update_refs_in_progress;
}

void ShenandoahHeap::post_allocation_collector_specific_setup(HeapWord* hw) {
  oop obj = oop(hw);

  // Assuming for now that objects can't be created already locked
  assert(! obj->has_displaced_mark(), "hopefully new objects don't have displaced mark");
  // tty->print_cr("post_allocation_collector_specific_setup:: "PTR_FORMAT, p2i(obj));

  if (_concurrent_mark_in_progress || (ShenandoahUpdateRefsEarly && _evacuation_in_progress)) {
    mark_current_no_checks(obj);
  }
}

/*
 * Marks the object. Returns true if the object has not been marked before and has
 * been marked by this thread. Returns false if the object has already been marked,
 * or if a competing thread succeeded in marking this object.
 */
bool ShenandoahHeap::mark_current(oop obj) const {
#ifdef ASSERT
  if (obj != oopDesc::bs()->resolve_oop(obj)) {
    tty->print_cr("heap region containing obj:");
    ShenandoahHeapRegion* obj_region = heap_region_containing(obj);
    obj_region->print();
    tty->print_cr("obj has been marked prev: %s", BOOL_TO_STR(is_marked_prev(obj)));
    tty->print_cr("heap region containing forwardee:");
    ShenandoahHeapRegion* forward_region = heap_region_containing(oopDesc::bs()->resolve_oop(obj));
    forward_region->print();    
    tty->print_cr("fwd has been marked prev: %s", BOOL_TO_STR(is_marked_prev(oopDesc::bs()->resolve_oop(obj))));
  }
#endif

  assert(obj == oopDesc::bs()->resolve_oop(obj), "only mark forwarded copy of objects");
  return mark_current_no_checks(obj);
}

bool ShenandoahHeap::mark_current_no_checks(oop obj) const {
  return _next_mark_bit_map->parMark((HeapWord*) obj);
}

bool ShenandoahHeap::isMarkedCurrent(oop obj) const {
  return _next_mark_bit_map->isMarked((HeapWord*) obj);
}

bool ShenandoahHeap::is_marked_prev(oop obj) const {
  return _prev_mark_bit_map->isMarked((HeapWord*) obj);
}

void ShenandoahHeap::verify_copy(oop p,oop c){
    assert(p != oopDesc::bs()->resolve_oop(p), "forwarded correctly");
    assert(oopDesc::bs()->resolve_oop(p) == c, "verify pointer is correct");
    if (p->klass() != c->klass()) {
      print_heap_regions();
    }
    assert(p->klass() == c->klass(), err_msg("verify class p-size: "INT32_FORMAT" c-size: "INT32_FORMAT, p->size(), c->size()));
    assert(p->size() == c->size(), "verify size");
    // Object may have been locked between copy and verification
    //    assert(p->mark() == c->mark(), "verify mark");
    assert(c == oopDesc::bs()->resolve_oop(c), "verify only forwarded once");
  }

void ShenandoahHeap::oom_during_evacuation() {
  // tty->print_cr("Out of memory during evacuation, cancel evacuation, schedule full GC");
  // We ran out of memory during evacuation. Cancel evacuation, and schedule a full-GC.
  collector_policy()->set_should_clear_all_soft_refs(true);
  concurrent_thread()->schedule_full_gc();
  cancel_evacuation();
}

void ShenandoahHeap::copy_object(oop p, HeapWord* s) {
  HeapWord* filler = s;
  assert(s != NULL, "allocation of brooks pointer must not fail");
  HeapWord* copy = s + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;

  guarantee(copy != NULL, "allocation of copy object must not fail");
  Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
  initialize_brooks_ptr(filler, copy);

#ifdef ASSERT
  if (ShenandoahTraceEvacuations) {
    tty->print_cr("copy object from "PTR_FORMAT" to: "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy));
  }
#endif
}

HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
  HeapWord* obj = thread->tlab().allocate(size);
  if (obj != NULL) {
    return obj;
  }
  // Otherwise...
  return allocate_from_gclab_slow(thread, size);
}

HeapWord* ShenandoahHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // Retain tlab and allocate object in shared space if
  // the amount free in the tlab is too large to discard.
  if (thread->tlab().free() > thread->tlab().refill_waste_limit()) {
    thread->tlab().record_slow_allocation(size);
    return NULL;
  }

  // Discard tlab and allocate a new one.
  // To minimize fragmentation, the last TLAB may be smaller than the rest.
  size_t new_gclab_size = thread->tlab().compute_size(size);

  thread->tlab().clear_before_allocation();

  if (new_gclab_size == 0) {
    return NULL;
  }

  // Allocate a new TLAB...
  HeapWord* obj = allocate_new_gclab(new_gclab_size);
  if (obj == NULL) {
    return NULL;
  }
  fill_with_object(obj, size);

  if (ZeroTLAB) {
    // ..and clear it.
    Copy::zero_to_words(obj, new_gclab_size);
  } else {
    // ...and zap just allocated object.
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(obj + hdr_size, new_gclab_size - hdr_size, badHeapWordVal);
#endif // ASSERT
  }
  thread->tlab().fill(obj, obj + size, new_gclab_size);
  return obj;
}

oop ShenandoahHeap::evacuate_object(oop p, Thread* thread) {
  ShenandoahHeapRegion* hr;
  size_t required;

#ifdef ASSERT
  if (ShenandoahVerifyReadsToFromSpace) {
    hr = heap_region_containing(p);
    {
      hr->memProtectionOff();    
      required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
      hr->memProtectionOn();    
    }
  } else {
    required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
  }
#else
    required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
#endif

  assert(! heap_region_containing(p)->is_humonguous(), "never evacuate humonguous objects");

  bool alloc_from_gclab = true;
  HeapWord* filler = allocate_from_gclab(thread, required);
  if (filler == NULL) {
    filler = allocate_memory(required, true);
    alloc_from_gclab = false;
  }

  if (filler == NULL) {
    oom_during_evacuation();
    // If this is a Java thread, it should have waited
    // until all GC threads are done, and then we
    // return the forwardee.
    oop resolved = ShenandoahBarrierSet::resolve_oop_static(p);
    return resolved;
  }

  HeapWord* copy = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  
#ifdef ASSERT
  if (ShenandoahVerifyReadsToFromSpace) {
    hr->memProtectionOff();
    copy_object(p, filler);
    hr->memProtectionOn();
  } else {
    copy_object(p, filler);    
  }
#else 
    copy_object(p, filler);    
#endif

  if (ShenandoahUpdateRefsEarly) {
    mark_current(oop(copy));
  }


  HeapWord* result = BrooksPointer::get(p).cas_forwardee((HeapWord*) p, copy);

  oop return_val;
  if (result == (HeapWord*) p) {
    return_val = oop(copy);

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print("Copy of "PTR_FORMAT" to "PTR_FORMAT" succeeded \n", p2i((HeapWord*) p), p2i(copy));
    }
#endif
  }  else {
    if (alloc_from_gclab) {
      thread->tlab().rollback(required);
    }
#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print_cr("Copy of "PTR_FORMAT" to "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy));
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
      tty->print_cr("allocating new region at index: "INT32_FORMAT, new_region_index);
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

  size_t num_regions = _num_regions;
  size_t new_num_regions = num_regions + new_regions;
  if (new_num_regions >= _max_regions) {
    // Not enough regions left.
    return -1;
  }

  size_t expand_size = new_regions * ShenandoahHeapRegion::RegionSizeBytes;
  if (ShenandoahGCVerbose) {
    tty->print_cr("expanding storage by "SIZE_FORMAT_HEX" bytes, for "INT32_FORMAT" new regions", expand_size, new_regions);
  }
  bool success = _storage.expand_by(expand_size);
  assert(success, "should always be able to expand by requested size");

  _num_regions = new_num_regions;

  return num_regions;

}

#ifndef CC_INTERP
void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
  __ incrementq(obj, BrooksPointer::BROOKS_POINTER_OBJ_SIZE * HeapWordSize);
  __ movptr(Address(obj, -1 * HeapWordSize), obj);
}
#endif

bool  ShenandoahIsAliveClosure:: do_object_b(oop obj) { 

  if (! ShenandoahUpdateRefsEarly) {
    obj = ShenandoahBarrierSet::resolve_oop_static(obj);
  }
#ifdef ASSERT
  if (obj != ShenandoahBarrierSet::resolve_oop_static(obj)) {
    ShenandoahHeap* sh = ShenandoahHeap::heap();
    tty->print_cr("from-space object is marked: %s", BOOL_TO_STR(sh->is_marked_current(obj)));
    tty->print_cr("to-space object is marked: %s", BOOL_TO_STR(sh->is_marked_current(ShenandoahBarrierSet::resolve_oop_static(obj))));
  }
#endif
  assert(obj == ShenandoahBarrierSet::resolve_oop_static(obj), "needs to be in to-space");

    HeapWord* addr = (HeapWord*) obj;
    ShenandoahHeap* sh = ShenandoahHeap::heap();

    if (ShenandoahTraceWeakReferences) {

      if (addr != NULL) {
	if(sh->is_in(addr)) {
	  if (sh->is_obj_ill(obj)) {
	    HandleMark hm;
	    tty->print_cr("ShenandoahIsAliveClosure Found an ill object "PTR_FORMAT, p2i((HeapWord*) obj));
	    obj->print();
	  }
	  else 
	    tty->print_cr("found a healthy object "PTR_FORMAT, p2i((HeapWord*) obj));

	} else {
	  tty->print_cr("found an object outside the heap "PTR_FORMAT, p2i((HeapWord*) obj));
	}
      } else {
	tty->print_cr("found a null object "PTR_FORMAT, p2i((HeapWord*) obj));
      }
    }

    return addr != NULL && sh->is_marked_current(obj); //(!sh->is_in(addr) || !sh->is_obj_ill(obj));
}

void ShenandoahHeap::ref_processing_init() {
  SharedHeap::ref_processing_init();
  MemRegion mr = reserved_region();

  // Concurrent Mark ref processor
  _ref_processor_cm =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ParallelGCThreads > 1),
                                // mt processing
                           (int) ParallelGCThreads,
                                // degree of mt processing
                           (ParallelGCThreads > 1) || (ConcGCThreads > 1),
                                // mt discovery
                           (int) MAX2(ParallelGCThreads, ConcGCThreads),
                                // degree of mt discovery
                           false,
                                // Reference discovery is not atomic
			   &isAlive);
                                // is alive closure
                                // (for efficiency/performance)
}

#ifdef ASSERT
void ShenandoahHeap::set_from_region_protection(bool protect) {
  for (uint i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* region = _ordered_regions[i];
    if (region != NULL && region->is_in_collection_set()) {
      if (protect) {
        region->memProtectionOn();
      } else {
        region->memProtectionOff();
      }
    }
  }
}
#endif

void ShenandoahHeap::acquire_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::acquirePLL);
}

void ShenandoahHeap::release_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}

ShenandoahHeapRegion** ShenandoahHeap::heap_regions() {
  return _ordered_regions;
}

size_t ShenandoahHeap::num_regions() {
  return _num_regions;
}

size_t ShenandoahHeap::max_regions() {
  return _max_regions;
}

GCTracer* ShenandoahHeap::tracer() {
  return _tracer;
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  // This is used for stats. Dunno how to easily track this.
  return used();
  return 0;
}

void ShenandoahHeap::cancel_evacuation() {
  _cancelled_evacuation = true;
  // If this is a GC thread, we let it return immediately, otherwise we wait
  // until all GC threads are done.
  if (! Thread::current()->is_GC_task_thread()) {
    while (! workers()->is_idle()) { // wait.
      Thread::current()->_ParkEvent->park(1) ;
    }
  }
}

bool ShenandoahHeap::cancelled_evacuation() {
  bool cancelled = _cancelled_evacuation;
  return cancelled;
}

int ShenandoahHeap::max_workers() {
  return _max_workers;
}

void ShenandoahHeap::shutdown() {
  // We set this early here, to let GC threads terminate before we ask the concurrent thread
  // to terminate, which would otherwise block until all GC threads come to finish normally.
  _cancelled_evacuation = true;
  _concurrent_gc_thread->shutdown();
  cancel_evacuation();
}

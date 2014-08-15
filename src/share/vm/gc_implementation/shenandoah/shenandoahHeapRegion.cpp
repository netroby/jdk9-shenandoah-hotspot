/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/universe.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"

size_t ShenandoahHeapRegion::RegionSizeShift = 23;
size_t ShenandoahHeapRegion::RegionSizeBytes = 1 << ShenandoahHeapRegion::RegionSizeShift; // 1024 * 1024 * 8;

jint ShenandoahHeapRegion::initialize(HeapWord* start, 
				      size_t regionSizeWords, int index) {

  reserved = MemRegion((HeapWord*) start, regionSizeWords);
  ContiguousSpace::initialize(reserved, true, false);
  liveData = 0;
  _is_in_collection_set = false;
  _region_number = index;
#ifdef ASSERT
  _mem_protection_level = 1; // Off, level 1.
#endif
  return JNI_OK;
}

int ShenandoahHeapRegion::region_number() {
  return _region_number;
}

bool ShenandoahHeapRegion::rollback_allocation(uint size) {
  set_top(top() - size);
  return true;
}

void ShenandoahHeapRegion::clearLiveData() {
  setLiveData(0);
}

void ShenandoahHeapRegion::setLiveData(size_t s) {
  Atomic::store_ptr(s, (intptr_t*) &liveData);
}

void ShenandoahHeapRegion::increase_live_data(size_t s) {
  Atomic::add_ptr(s, (intptr_t*) &liveData);
}

size_t ShenandoahHeapRegion::getLiveData() {
  return liveData;
}

size_t ShenandoahHeapRegion::garbage() {
  assert(used() >= getLiveData() || is_humonguous(), err_msg("Live Data must be a subset of used() live: %d used: %d", getLiveData(), used()));
  size_t result = used() - getLiveData();
  return result;
}

bool ShenandoahHeapRegion::is_in_collection_set() {
  return _is_in_collection_set;
}

#include <sys/mman.h>

#ifdef ASSERT

void ShenandoahHeapRegion::memProtectionOn() {
  /*
  tty->print_cr("protect memory on region level: %d", _mem_protection_level);
  print(tty);
  */
  MutexLockerEx ml(ShenandoahMemProtect_lock, true);
  assert(_mem_protection_level >= 1, "invariant");

  if (--_mem_protection_level == 0) {
    if (ShenandoahVerifyWritesToFromSpace) {
      assert(! ShenandoahVerifyReadsToFromSpace, "can't verify from-space reads when verifying from-space writes");
      os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_READ);
    } else {
      assert(ShenandoahVerifyReadsToFromSpace, "need to be verifying reads here");
      assert(! ShenandoahConcurrentEvacuation, "concurrent evacuation needs to be turned off for verifying from-space-reads");
      os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_NONE);
    }
  }
}

void ShenandoahHeapRegion::memProtectionOff() {
  /*
  tty->print_cr("unprotect memory on region level: %d", _mem_protection_level);
  print(tty);
  */
  MutexLockerEx ml(ShenandoahMemProtect_lock, true);
  assert(_mem_protection_level >= 0, "invariant");
  if (_mem_protection_level++ == 0) {
    os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_RW);
  }
}

#endif

void ShenandoahHeapRegion::set_is_in_collection_set(bool b) {
  assert(! is_humonguous(), "never ever enter a humonguous region into the collection set");

  _is_in_collection_set = b;

#ifdef ASSERT
  if (ShenandoahVerifyWritesToFromSpace || ShenandoahVerifyReadsToFromSpace) {    
    if (b) {
      memProtectionOn();
      assert(_mem_protection_level == 0, "need to be protected here");
    } else {
      assert(_mem_protection_level == 0, "need to be protected here");
      memProtectionOff();
    }
  }
#endif
}

ByteSize ShenandoahHeapRegion::is_in_collection_set_offset() {
  return byte_offset_of(ShenandoahHeapRegion, _is_in_collection_set);
}

void ShenandoahHeapRegion::print(outputStream* st) {
  st->print("ShenandoahHeapRegion: %p/%d ", this, _region_number);

  if (is_in_collection_set())
    st->print("C");
  if (is_humonguous_start()) {
    st->print("H");
  }
  if (is_humonguous_continuation()) {
    st->print("h");
  }
  //else
    st->print(" ");

  st->print("live = %u garbage = %u bottom = %p end = %p top = %p\n", 
            getLiveData(), garbage(), bottom(), end(), top());
}


class SkipUnreachableObjectToOopClosure: public ObjectClosure {
  ExtendedOopClosure* _cl;
  bool _skip_unreachable_objects;
  ShenandoahHeap* _heap;

public:
  SkipUnreachableObjectToOopClosure(ExtendedOopClosure* cl, bool skip_unreachable_objects) :
    _cl(cl), _skip_unreachable_objects(skip_unreachable_objects), _heap(ShenandoahHeap::heap()) {}
  
  void do_object(oop obj) {
    
    if ((! _skip_unreachable_objects) || _heap->isMarkedCurrent(obj)) {
      if (_skip_unreachable_objects) {
        assert(_heap->isMarkedCurrent(obj), "obj must be live");
      }
      obj->oop_iterate(_cl);
    }
    
  }
};

void ShenandoahHeapRegion::object_iterate(ObjectClosure* blk) {
  HeapWord* p = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  while (p < top()) {
    blk->do_object(oop(p));
#ifdef ASSERT
    if (ShenandoahVerifyReadsToFromSpace) {
      memProtectionOff();
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
      memProtectionOn();
    } else {
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
#else
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
#endif
  }
}

HeapWord* ShenandoahHeapRegion::object_iterate_careful(ObjectClosureCareful* blk) {
  HeapWord * limit = concurrent_iteration_safe_limit();
  assert(limit <= top(), "sanity check");
  for (HeapWord* p = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE; p < limit;) {
    size_t size = blk->do_object_careful(oop(p));
    if (size == 0) {
      return p;  // failed at p
    } else {
      p += size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
  }
  return NULL; // all done
}

void ShenandoahHeapRegion::oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects) {
  SkipUnreachableObjectToOopClosure cl2(cl, skip_unreachable_objects);
  object_iterate(&cl2);
}

void ShenandoahHeapRegion::fill_region() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  
  if (free() > (BrooksPointer::BROOKS_POINTER_OBJ_SIZE + CollectedHeap::min_fill_size())) {
    HeapWord* filler = allocate(BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
    HeapWord* obj = allocate(end() - top());
    sh->fill_with_object(obj, end() - obj);
    sh->initialize_brooks_ptr(filler, obj);
  } 
}

void ShenandoahHeapRegion::set_humonguous_start(bool start) {
  _humonguous_start = start;
}

void ShenandoahHeapRegion::set_humonguous_continuation(bool continuation) {
  _humonguous_continuation = continuation;
}

bool ShenandoahHeapRegion::is_humonguous() {
  return _humonguous_start || _humonguous_continuation;
}

bool ShenandoahHeapRegion::is_humonguous_start() {
  return _humonguous_start;
}

bool ShenandoahHeapRegion::is_humonguous_continuation() {
  return _humonguous_continuation;
}

void ShenandoahHeapRegion::do_reset() {
  Space::initialize(reserved, true, false);
  clearLiveData();
  _humonguous_start = false;
  _humonguous_continuation = false;
}

void ShenandoahHeapRegion::recycle() {
  do_reset();
  set_is_in_collection_set(false);
}

void ShenandoahHeapRegion::reset() {
  assert(_mem_protection_level == 1, "needs to be unprotected here");
  do_reset();
  _is_in_collection_set = false;
}

HeapWord* ShenandoahHeapRegion::block_start_const(const void* p) const {
  assert(MemRegion(bottom(), end()).contains(p),
         err_msg("p (" PTR_FORMAT ") not in space [" PTR_FORMAT ", " PTR_FORMAT ")",
                  p, bottom(), end()));
  if (p >= top()) {
    return top();
  } else {
    HeapWord* last = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    HeapWord* cur = last;
    while (cur <= p) {
      last = cur;
      cur += oop(cur)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
    assert(oop(last)->is_oop(),
           err_msg(PTR_FORMAT " should be an object start", last));
    return last;
  }
}

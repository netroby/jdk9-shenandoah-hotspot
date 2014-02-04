/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/universe.hpp"

size_t ShenandoahHeapRegion::RegionSizeShift = 23;
size_t ShenandoahHeapRegion::RegionSizeBytes = 1 << ShenandoahHeapRegion::RegionSizeShift; // 1024 * 1024 * 8;

jint ShenandoahHeapRegion::initialize(HeapWord* start, 
				      size_t regionSizeWords, int index) {

  reserved = MemRegion((HeapWord*) start, regionSizeWords);
  ContiguousSpace::initialize(reserved, true, false);
  liveData = 0;
  _dirty = false;
  _region_number = index;
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
  if (is_humonguous()) {
    return RegionSizeBytes;
  } else {
    return liveData;
  }
}

size_t ShenandoahHeapRegion::garbage() {
  assert(used() >= getLiveData(), "Live Data must be a subset of used()");
  size_t result = used() - getLiveData();
  return result;
}

void ShenandoahHeapRegion::set_dirty(bool dirty) {
  _dirty = dirty;
}

bool ShenandoahHeapRegion::is_dirty() {
  return _dirty;
}

bool ShenandoahHeapRegion::claim() {
  bool previous = Atomic::cmpxchg(true, &claimed, false);
  return !previous;
}

void ShenandoahHeapRegion::clearClaim() {
  claimed = false;
}

bool ShenandoahHeapRegion::is_in_collection_set() {
  return _is_in_collection_set;
}

void ShenandoahHeapRegion::set_is_in_collection_set(bool b) {
  _is_in_collection_set = b;
}

bool ShenandoahHeapRegion::is_current_allocation_region() {
  return _is_current_allocation_region;
}

void ShenandoahHeapRegion::set_is_current_allocation_region(bool b) {
  _is_current_allocation_region = b;
}

ByteSize ShenandoahHeapRegion::is_in_collection_set_offset() {
  return byte_offset_of(ShenandoahHeapRegion, _is_in_collection_set);
}

void ShenandoahHeapRegion::print(outputStream* st) {
  st->print("ShenandoahHeapRegion: %p/%d ", this, _region_number);

  if (is_current_allocation_region()) 
    st->print("A");
  else if (is_in_collection_set())
    st->print("C");
  else if (is_dirty())
    st->print("D");
  else
    st->print(" ");

  st->print("live = %u garbage = %u claimed = %d bottom = %p end = %p top = %p dirty: %d active_tlabs: %d\n", 
            getLiveData(), garbage(), claimed, bottom(), end(), top(), _dirty, active_tlab_count);
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

void ShenandoahHeapRegion::increase_active_tlab_count() {
  assert(active_tlab_count >= 0, "never have negative tlab count");
  Atomic::inc(&active_tlab_count);
}

void ShenandoahHeapRegion::decrease_active_tlab_count() {
  Atomic::dec(&active_tlab_count);
  assert(active_tlab_count >= 0, err_msg("never have negative tlab count %d", active_tlab_count));
}

bool ShenandoahHeapRegion::has_active_tlabs() {
  assert(active_tlab_count >= 0, "never have negative tlab count");
  return active_tlab_count != 0;
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

void ShenandoahHeapRegion::recycle() {
  Space::initialize(reserved, true, false);
  clearLiveData();
  set_dirty(false);
  _humonguous_start = false;
  _humonguous_continuation = false;
}

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacuation.hpp"
#include "gc_implementation/shenandoah/shenandoahAllocRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

GCLABAllocator::GCLABAllocator(ShenandoahAllocRegion* region) :
  _region(region),
  _waste(0),
  _fallback(HeapAllocator()),
  _last_alloc_in_gclab(false) {
}

HeapWord* GCLABAllocator::allocate(size_t size) {

  HeapWord* obj;

  if (size < _region->space_available()) {
    if (ShenandoahGCVerbose) {
      tty->print("PEROC: %d size < _region->space_available() = %d\n", 
                 size, _region->space_available());
    }
    obj = _region->allocate(size);
    _last_alloc_in_gclab = true;
  } else if (size < _region->region_size()) {
    if (ShenandoahGCVerbose) {
      tty->print("PEROC: %d size < _region->region_size = %d\n ", size, _region->region_size());
    }
    _waste += _region->space_available();
    _region->fill_region();
    _region->allocate_new_region();
    obj = _region->allocate(size);
    _last_alloc_in_gclab = true;
  } else if (size < ShenandoahHeapRegion::RegionSizeBytes) {
    if (ShenandoahGCVerbose) {
      tty->print("PEROC: %d size < ShenandoahHeapRegion::RegionSizeBytes = %d\n ", 
                 size, ShenandoahHeapRegion::RegionSizeBytes);
    }
    _waste += _region->space_available();
    _region->fill_region();
    obj = _fallback.allocate(size);
    _last_alloc_in_gclab = false;
  } else {
    tty->print("PEROC:%d don't handle humongous objects\n", size);
    assert(false, "Don't handle humongous objects yet");
  }
  return obj;
}

void GCLABAllocator::rollback(HeapWord* filler, size_t size) {

  if (_last_alloc_in_gclab) {
    _region->rollback(size);
  } else {
    _fallback.rollback(filler, size);
  }
}

size_t GCLABAllocator::waste() {
  return _waste;
}

HeapAllocator::HeapAllocator() :
  _heap(ShenandoahHeap::heap()) {
}

HeapWord* HeapAllocator::allocate(size_t size) {
  return _heap->allocate_memory_gclab(size);
}

void HeapAllocator::rollback(HeapWord* filler, size_t size) {
  HeapWord* obj = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  _heap->initialize_brooks_ptr(filler, obj);
  _heap->fill_with_object(obj, size - BrooksPointer::BROOKS_POINTER_OBJ_SIZE, true);
}

TLABAllocator::TLABAllocator() :
  _last_alloc_in_tlab(false),
  _fallback(HeapAllocator()) {
}

HeapWord* TLABAllocator::allocate(size_t size) {

  HeapWord* result = NULL;
  if (UseTLAB) {
    result = ShenandoahHeap::allocate_from_tlab_work(Thread::current(), size);
  }

  if (result == NULL) {
    result = _fallback.allocate(size);
    _last_alloc_in_tlab = false;
  } else {
    _last_alloc_in_tlab = true;
  }
  return result;
}

void TLABAllocator::rollback(HeapWord* obj, size_t size) {
  if (_last_alloc_in_tlab) {
    Thread::current()->tlab().rollback(size);
  } else {
    _fallback.rollback(obj, size);
  }
}


#include "gc_implementation/shenandoah/shenandoahAllocRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

ShenandoahAllocRegion::ShenandoahAllocRegion() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  // arbitrary for now
  _start = 0;
  _end = 0;
  _alignment_reserve = BROOKS_POINTER_OBJ_SIZE + CollectedHeap::min_fill_size();
  _hard_end = 0;
  allocRegionSize = 1024 * 4;
}

HeapWord* ShenandoahAllocRegion::allocate(size_t word_size) {
  if (word_size > allocRegionSize) {
    ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
    if (ShenandoahGCVerbose) 
      tty->print("Allocating new big object alloc region of size: %d\n", word_size);
    return sh->allocate_new_gclab(word_size);
  }

  if (_start + word_size < _end) {
    HeapWord* result = _start;
    _start = _start + word_size;
    return result;
  } else {
    fill_region();
    allocate_new_region();
    return allocate(word_size);
  }
}

void ShenandoahAllocRegion::print() {
  tty->print("AllocRegion: "PTR_FORMAT"_start = "PTR_FORMAT" end = "PTR_FORMAT"\n",
	     this, _start, _end);
}
  
void ShenandoahAllocRegion::allocate_new_region() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  _start = sh->allocate_new_gclab(allocRegionSize + _alignment_reserve);
  _end = _start + allocRegionSize;
  _hard_end = _end + _alignment_reserve;
}

void ShenandoahAllocRegion::fill_region() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  if (_start != 0) {
    if (ShenandoahGCVerbose) 
      tty->print("fill allocation region _start = %p _hard_end = %p\n", _start, _hard_end);
    if ((size_t)(_hard_end - _start) > (BROOKS_POINTER_OBJ_SIZE + CollectedHeap::min_fill_size())) {
      HeapWord* filler = _start;
      _start = _start + BROOKS_POINTER_OBJ_SIZE;
      CollectedHeap::fill_with_object(_start, _hard_end - _start);
      sh->initialize_brooks_ptr(filler, _start);
      _start = 0;
      _end = 0;
      _hard_end = 0;
    } else {
      assert(false, "Should have enough reserve space");
    }
  }
}

size_t ShenandoahAllocRegion::space_available() {
  return (size_t) ( _end - _start) ;
}

size_t ShenandoahAllocRegion::region_size() {
  return allocRegionSize;
}

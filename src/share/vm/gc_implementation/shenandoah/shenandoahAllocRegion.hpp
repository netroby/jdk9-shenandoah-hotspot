#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHALLOCREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHALLOCREGION_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

class ShenandoahAllocRegion {
private:
  uint allocRegionSize;
  HeapWord* _start;
  // We leave enough room at the end of an allocRegion to be able to 
  // insert a filler object.  So _hard_end is bigger than _end.
  HeapWord* _end;
  HeapWord* _hard_end;
  size_t _alignment_reserve;

public:
  HeapWord* allocate(size_t word_size);
  void rollback(size_t word_size);
  void allocate_new_region();
  void fill_region();
  void print();
  size_t space_available();
  size_t region_size();
  ShenandoahAllocRegion();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHALLOCREGION_HPP

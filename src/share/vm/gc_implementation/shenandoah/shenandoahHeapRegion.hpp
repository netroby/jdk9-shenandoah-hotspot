#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.inline.hpp"
#include "memory/universe.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {
public:
   ShenandoahHeapRegion* _next;
   int regionNumber;
   static size_t RegionSizeBytes;


   jint initialize(HeapWord* start, size_t regionSize);
   void setNext(ShenandoahHeapRegion* next) {
      _next = next;
   }
   ShenandoahHeapRegion* next() {
      return _next;
   }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

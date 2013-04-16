#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.inline.hpp"
#include "memory/universe.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {
public:
   ShenandoahHeapRegion* _next;
   int regionNumber;
   static size_t RegionSizeBytes;
   MemRegion reserved;


   jint initialize(HeapWord* start, size_t regionSize);
   void setNext(ShenandoahHeapRegion* next) {
      _next = next;
   }
   ShenandoahHeapRegion* next() {
      return _next;
   }

  // Roll back the previous allocation of an object with specified size.
  // Returns TRUE when successful, FALSE if not successful or not supported.
  bool rollback_allocation(uint size);
};



#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

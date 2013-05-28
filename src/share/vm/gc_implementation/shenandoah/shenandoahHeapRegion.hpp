#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.inline.hpp"
#include "memory/universe.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {
public:
   int regionNumber;
   static size_t RegionSizeBytes;
   size_t liveData;
   MemRegion reserved;
   volatile unsigned int claimed;

   ShenandoahHeapRegion* _next;

private:
  bool _dirty;
public:
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
  void clearLiveData() {liveData = 0;}
  void setLiveData(size_t s) {liveData = s;}
  size_t getLiveData() { return liveData;}

  void print();
  size_t garbage() {
    return used() - liveData;
  }

  bool is_dirty() {
    return _dirty;
  }

  void printDetails() {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x live = %x\n", 
	       regionNumber,top(), used(), free(), getLiveData());
  }

  void set_dirty(bool dirty) {
    _dirty = dirty;
  }

  void recycle() {
    Space::initialize(reserved, true, false);
    clearLiveData();
  }

  bool claim() {
    bool previous = Atomic::cmpxchg(true, &claimed, false);
    return !previous;
  }

  void clearClaim() {
    claimed = false;
  }

  void oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects);

  
};



#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

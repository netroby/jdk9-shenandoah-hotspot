#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.inline.hpp"
#include "memory/universe.hpp"
#include "utilities/sizes.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {
public:
   int regionNumber;
   static size_t RegionSizeBytes;
   static size_t RegionSizeShift;
   volatile jlong liveData;
   MemRegion reserved;
   volatile unsigned int claimed;

private:
  bool _dirty;
  bool _is_in_collection_set;
  bool _is_current_allocation_region;
  volatile jint active_tlab_count;

  bool _humonguous_start;
  bool _humonguous_continuation;

public:
  jint initialize(HeapWord* start, size_t regionSize, int index);


  // Roll back the previous allocation of an object with specified size.
  // Returns TRUE when successful, FALSE if not successful or not supported.
  bool rollback_allocation(uint size);

  void clearLiveData() { setLiveData(0);}
  void setLiveData(jlong s) {
    Atomic::store(s, &liveData);
  }
  void increase_live_data(jlong s) {
    Atomic::add(s, &liveData);
  }

  size_t getLiveData() { return liveData;}

  void print(outputStream* st = tty);

  size_t garbage() {
    size_t result = used() - liveData;
    assert(result >= 0, "Live Data must be a subset of used()");
    return result;
  }

  void set_dirty(bool dirty) {
    _dirty = dirty;
  }

  bool is_dirty() {
    return _dirty;
  }

  void printDetails() {
    tty->print("Region %d top = "PTR_FORMAT" used = %x free = %x live = %x\n", 
	       regionNumber,top(), used(), free(), getLiveData());
  }

  void recycle();

  bool claim() {
    bool previous = Atomic::cmpxchg(true, &claimed, false);
    return !previous;
  }

  void clearClaim() {
    claimed = false;
  }

  void oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects);

  // Just before GC we need to fill the current region.
  void fill_region();

  bool is_in_collection_set() {
    return _is_in_collection_set;
  }

  void set_is_in_collection_set(bool b) {
    _is_in_collection_set = b;
  }

  bool is_current_allocation_region() {
    return _is_current_allocation_region;
  }

  void set_is_current_allocation_region(bool b) {
    _is_current_allocation_region = b;
  }

  void set_humonguous_start(bool start);
  void set_humonguous_continuation(bool continuation);

  bool is_humonguous();
  bool is_humonguous_start();
  bool is_humonguous_continuation();

  void increase_active_tlab_count();
  void decrease_active_tlab_count();
  bool has_active_tlabs();

  static ByteSize is_in_collection_set_offset() { return byte_offset_of(ShenandoahHeapRegion, _is_in_collection_set); }
};



#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

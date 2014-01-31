/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.inline.hpp"
#include "memory/universe.hpp"
#include "utilities/sizes.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {

public:
  static size_t RegionSizeBytes;
  static size_t RegionSizeShift;

private:
  int _region_number;
  volatile size_t liveData;
  MemRegion reserved;
  volatile unsigned int claimed;
  bool _dirty;
  bool _is_in_collection_set;
  bool _is_current_allocation_region;
  volatile jint active_tlab_count;

  bool _humonguous_start;
  bool _humonguous_continuation;

public:
  jint initialize(HeapWord* start, size_t regionSize, int index);


  int region_number();

  // Roll back the previous allocation of an object with specified size.
  // Returns TRUE when successful, FALSE if not successful or not supported.
  bool rollback_allocation(uint size);

  void clearLiveData();
  void setLiveData(size_t s);
  void increase_live_data(size_t s);

  size_t getLiveData();

  void print(outputStream* st = tty);

  size_t garbage();

  void set_dirty(bool dirty);

  bool is_dirty();

  void recycle();

  bool claim();

  void clearClaim();

  void oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects);

  // Just before GC we need to fill the current region.
  void fill_region();

  bool is_in_collection_set();

  void set_is_in_collection_set(bool b);

  bool is_current_allocation_region();

  void set_is_current_allocation_region(bool b);

  void set_humonguous_start(bool start);
  void set_humonguous_continuation(bool continuation);

  bool is_humonguous();
  bool is_humonguous_start();
  bool is_humonguous_continuation();

  void increase_active_tlab_count();
  void decrease_active_tlab_count();
  bool has_active_tlabs();

  static ByteSize is_in_collection_set_offset();
};



#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.hpp"
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
  bool _is_in_collection_set;

  bool _humonguous_start;
  bool _humonguous_continuation;

#ifdef ASSERT
  int _mem_protection_level;
#endif

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

  void recycle();
  void reset();

  void oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects);

  void object_iterate(ObjectClosure* blk, bool allow_cancel);

  HeapWord* object_iterate_careful(ObjectClosureCareful* cl);

  HeapWord* block_start_const(const void* p) const;

  // Just before GC we need to fill the current region.
  void fill_region();

  bool is_in_collection_set();

  void set_is_in_collection_set(bool b);

  void set_humonguous_start(bool start);
  void set_humonguous_continuation(bool continuation);

  bool is_humonguous();
  bool is_humonguous_start();
  bool is_humonguous_continuation();

#ifdef ASSERT
  void memProtectionOn();
  void memProtectionOff();
#endif

  static ByteSize is_in_collection_set_offset();

private:
  void do_reset();
};



#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

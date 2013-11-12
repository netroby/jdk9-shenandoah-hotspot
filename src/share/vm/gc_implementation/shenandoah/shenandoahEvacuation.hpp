#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHEVACUATION_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHEVACUATION_HPP

#include <cstddef>

class HeapWord;
class ShenandoahAllocRegion;
class ShenandoahHeap;

/**
 * The allocator is used for allocating the copies of objects during
 * evacuation.
 */
class EvacuationAllocator {
public:
  virtual HeapWord* allocate(size_t size) = 0;
  virtual void rollback(HeapWord* obj, size_t size) = 0;
};

class HeapAllocator : public EvacuationAllocator {

private:
  ShenandoahHeap* _heap;

public:
  HeapAllocator();
  HeapWord* allocate(size_t size);
  void rollback(HeapWord* obj, size_t size);
};

class GCLABAllocator : public EvacuationAllocator {

private:
  ShenandoahAllocRegion* _region;
  HeapAllocator _fallback;
  size_t _waste;
  bool _last_alloc_in_gclab;

public:
  GCLABAllocator(ShenandoahAllocRegion* region);
  HeapWord* allocate(size_t size);
  void rollback(HeapWord* obj, size_t size);
  size_t waste();
};

class TLABAllocator : public EvacuationAllocator {

private:
  HeapAllocator _fallback;
  bool _last_alloc_in_tlab;

public:
  TLABAllocator();
  HeapWord* allocate(size_t size);
  void rollback(HeapWord* obj, size_t size);
};


#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHEVACUATION_HPP

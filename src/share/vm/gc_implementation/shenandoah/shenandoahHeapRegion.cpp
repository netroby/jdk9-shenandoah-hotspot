#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/universe.hpp"

size_t ShenandoahHeapRegion::RegionSizeBytes = 1024 * 1024 * 8;

jint ShenandoahHeapRegion::initialize(HeapWord* start, 
				      size_t regionSizeWords) {
  reserved = MemRegion((HeapWord*) start, regionSizeWords);
  ContiguousSpace::initialize(reserved, true, false);
  liveData = 0;
  _dirty = false;
  return JNI_OK;
}

bool ShenandoahHeapRegion::rollback_allocation(uint size) {
  set_top(top() - size);
  return true;
}

void ShenandoahHeapRegion::print() {
  tty->print("ShenandoahHeapRegion: %d live = %d garbage = %d claimed = %d\n", regionNumber, liveData, garbage(), claimed);
}



class SkipUnreachableObjectToOopClosure: public ObjectClosure {
  ExtendedOopClosure* _cl;
  bool _skip_unreachable_objects;
  ShenandoahHeap* _heap;

public:
  SkipUnreachableObjectToOopClosure(ExtendedOopClosure* cl, bool skip_unreachable_objects) :
    _cl(cl), _skip_unreachable_objects(skip_unreachable_objects), _heap(ShenandoahHeap::heap()) {}
  
  void do_object(oop obj) {
    
    if ((! _skip_unreachable_objects) || _heap->isMarkedCurrent(obj)) {
      if (_skip_unreachable_objects) {
        assert(_heap->isMarkedCurrent(obj), "obj must be live");
      }
      obj->oop_iterate(_cl);
    }
    
  }
};

void ShenandoahHeapRegion::oop_iterate(ExtendedOopClosure* cl, bool skip_unreachable_objects) {
  SkipUnreachableObjectToOopClosure cl2(cl, skip_unreachable_objects);
  object_iterate(&cl2);
}

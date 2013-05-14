#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
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

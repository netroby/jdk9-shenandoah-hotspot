#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "memory/universe.hpp"

size_t ShenandoahHeapRegion::RegionSizeBytes = 1024 * 1024 * 8;

jint ShenandoahHeapRegion::initialize(HeapWord* start, 
				      size_t regionSizeWords) {
  reserved = MemRegion((HeapWord*) start, regionSizeWords);
  ContiguousSpace::initialize(reserved, true, false);
  return JNI_OK;
}

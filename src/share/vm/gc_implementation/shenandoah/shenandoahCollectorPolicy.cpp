#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "utilities/globalDefinitions.hpp"
// copied from G1
#define TARGET_REGION_NUMBER 2048
#define MIN_REGION_SIZE (1024 * 1024)

ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() {
  min_heap_size = Arguments::min_heap_size();
  region_size = MAX2(min_heap_size / TARGET_REGION_NUMBER,
				  (uintx) MIN_REGION_SIZE);
}



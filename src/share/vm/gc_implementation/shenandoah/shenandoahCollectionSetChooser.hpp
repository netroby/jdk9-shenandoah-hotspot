#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "utilities/growableArray.hpp"

class ShenandoahCollectionSetChooser {
private:
  GrowableArray<ShenandoahHeapRegion*> _regions;
  GrowableArray<ShenandoahHeapRegion*> _empty_regions;
  GrowableArray<ShenandoahHeapRegion*> _cs_regions;
  
  size_t garbage_threshold;
    
public:
  void initialize(ShenandoahHeapRegion* first);
  GrowableArray<ShenandoahHeapRegion*> empty_regions() { return _empty_regions;}
  GrowableArray<ShenandoahHeapRegion*> cs_regions() { return _cs_regions;}
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP

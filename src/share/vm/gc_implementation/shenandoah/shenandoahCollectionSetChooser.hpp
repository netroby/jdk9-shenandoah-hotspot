#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "utilities/growableArray.hpp"

class ShenandoahCollectionSetChooser {
private:
    GrowableArray<ShenandoahHeapRegion*> _regions;
public:
    void initialize(ShenandoahHeapRegion* first);
    ShenandoahHeapRegion* get_next();
    ShenandoahHeapRegion* peek();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCOLLECTIONSETCHOOSER_HPP

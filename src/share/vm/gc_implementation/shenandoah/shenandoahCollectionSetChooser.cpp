#include "gc_implementation/shenandoah/shenandoahCollectionSetChooser.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"


int compareHeapRegions(ShenandoahHeapRegion** a, ShenandoahHeapRegion** b) {
  if (*a == NULL) {
    if (*b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (*b == NULL) {
    return -1;
  }

  size_t garbage_a = (*a)->garbage();
  size_t garbage_b = (*b)->garbage();
  
  if (garbage_a > garbage_b) 
    return -1;
  else if (garbage_a < garbage_b)
    return 1;
  else return 0;
}

void ShenandoahCollectionSetChooser::initialize(ShenandoahHeapRegion* first) {
  _regions.clear();
  ShenandoahHeapRegion* current = first;

  while (current != NULL) {
    _regions.append(current);
    current = current->next();
  }
  _regions.sort(compareHeapRegions);
  
  tty->print("printing sorted regions\n");
  for (int i = 0; i < _regions.length(); i++) {
    _regions.at(i)->print();
  }
}
  

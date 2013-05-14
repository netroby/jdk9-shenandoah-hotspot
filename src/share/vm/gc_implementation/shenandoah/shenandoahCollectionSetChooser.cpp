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
  garbage_threshold = ShenandoahHeapRegion::RegionSizeBytes/2;
  _regions.clear();
  _empty_regions.clear();
  _cs_regions.clear();

  ShenandoahHeapRegion* current = first;

  while (current != NULL) {
    _regions.append(current);
    current = current->next();
  }
  _regions.sort(compareHeapRegions);

  for (int i = 0; i < _regions.length(); i++) {
    if (_regions.at(i)->garbage() > garbage_threshold)
      _cs_regions.append(_regions.at(i));
    else if (_regions.at(i)->is_empty())
      _empty_regions.append(_regions.at(i));
  }
  

  if (_empty_regions.length() < _cs_regions.length())
    _cs_regions.trunc_to(_empty_regions.length());
    
  if (ShenandoahGCVerbose) {
  
    tty->print("printing sorted regions\n");
    for (int i = 0; i < _regions.length(); i++) {
      _regions.at(i)->print();
    }

    tty->print("printing collection set\n");
    for (int i = 0; i < _cs_regions.length(); i++) {
      _cs_regions.at(i)->print();
    }

    tty->print("printing empty regions\n");
    for (int i = 0; i < _empty_regions.length(); i++) {
      _empty_regions.at(i)->print();
    }
  }
  
}
  


#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP

#include "utilities/globalDefinitions.hpp"
#include "oops/oop.hpp"

class BrooksPointer {

private:

  uintptr_t* heap_word;

  BrooksPointer(uintptr_t* heap_word);

public:

  static const uintptr_t AGE_MASK = 0x7;
  static const uintptr_t FORWARDEE_MASK = ~AGE_MASK;

  uint get_age();
  void set_age(uint age);

  oop get_forwardee();
  void set_forwardee(oop forwardee);

  static BrooksPointer get(oop obj);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP

/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHRUNTIME_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHRUNTIME_HPP

#include "oops/oop.hpp"

class ShenandoahRuntime : AllStatic {
public:
  static bool compare_and_swap_object(HeapWord* adr, oopDesc* newval, oopDesc* expected);
};
#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHRUNTIME_HPP

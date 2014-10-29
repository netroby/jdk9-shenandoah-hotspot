/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/shenandoahRuntime.hpp"
#include "runtime/interfaceSupport.hpp"
#include "oops/oop.inline.hpp"

JRT_LEAF(bool, ShenandoahRuntime::compare_and_swap_object(HeapWord* addr, oopDesc* newval, oopDesc* old))
  bool success;
  oop expected;
  do {
    expected = old;
    old = oopDesc::atomic_compare_exchange_oop(newval, addr, expected, true);
    success  = (old == expected);
  } while ((! success) && oopDesc::bs()->resolve_oop(old) == oopDesc::bs()->resolve_oop(expected));

  return success;
JRT_END

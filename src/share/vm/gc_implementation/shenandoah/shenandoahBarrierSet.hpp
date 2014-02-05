/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "memory/barrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacuation.hpp"

class ShenandoahBarrierSet: public BarrierSet {
private:
  EvacuationAllocator* _allocator;

  oopDesc* get_shenandoah_forwardee_helper(oopDesc* p);


public:

  ShenandoahBarrierSet();

  void print_on(outputStream* st) const;

  bool is_a(BarrierSet::Name bsn);

  bool has_read_prim_array_opt();
  bool has_read_prim_barrier();
  bool has_read_ref_array_opt();
  bool has_read_ref_barrier();
  bool has_read_region_opt();
  bool has_write_prim_array_opt();
  bool has_write_prim_barrier();
  bool has_write_ref_array_opt();
  bool has_write_ref_barrier();
  bool has_write_ref_pre_barrier();
  bool has_write_region_opt();
  bool is_aligned(HeapWord* hw);
  void read_prim_array(MemRegion mr);
  void read_prim_field(HeapWord* hw, size_t s);
  bool read_prim_needs_barrier(HeapWord* hw, size_t s);
  void read_ref_array(MemRegion mr);

  void read_ref_field(void* v);

  bool read_ref_needs_barrier(void* v);
  void read_region(MemRegion mr);
  void resize_covered_region(MemRegion mr);
  void write_prim_array(MemRegion mr);
  void write_prim_field(HeapWord* hw, size_t s , juint x, juint y);
  bool write_prim_needs_barrier(HeapWord* hw, size_t s, juint x, juint y);
  void write_ref_array_work(MemRegion mr);

  template <class T> void
  write_ref_array_pre_work(T* dst, int count);

  void write_ref_array_pre(oop* dst, int count, bool dest_uninitialized);

  void write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized);


  template <class T> static void write_ref_field_pre_static(T* field, oop newVal);

  // We export this to make it available in cases where the static
  // type of the barrier set is known.  Note that it is non-virtual.
  template <class T> inline void inline_write_ref_field_pre(T* field, oop newVal);

  // These are the more general virtual versions.
  void write_ref_field_pre_work(oop* field, oop new_val);
  void write_ref_field_pre_work(narrowOop* field, oop new_val);
  void write_ref_field_pre_work(void* field, oop new_val);

  void write_ref_field_work(void* v, oop o);
  void write_region_work(MemRegion mr);

  oopDesc* get_shenandoah_forwardee(oopDesc* p);
  static bool is_brooks_ptr(oopDesc* p);
  static bool has_brooks_ptr(oopDesc* p);

  virtual oopDesc* resolve_oop(oopDesc* src);
  virtual oopDesc* maybe_resolve_oop(oopDesc* src);
  oopDesc* resolve_and_maybe_copy_oopHelper(oopDesc* src);
  oopDesc* resolve_and_maybe_copy_oop_work(oopDesc* src);
  virtual oopDesc* resolve_and_maybe_copy_oop(oopDesc* src);
  static oopDesc* resolve_and_maybe_copy_oop_static(oopDesc* src);

#ifndef CC_INTERP
  // TODO: The following should really live in an X86 specific subclass.
  virtual void compile_resolve_oop(MacroAssembler* masm, Register dst);
  virtual void compile_resolve_oop_not_null(MacroAssembler* masm, Register dst);
  void compile_resolve_oop_for_write(MacroAssembler* masm, Register dst, int num_save_state = 0, ...);
#endif
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

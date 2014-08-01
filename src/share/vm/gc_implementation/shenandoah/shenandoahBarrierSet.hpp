/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "memory/barrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

class ShenandoahBarrierSet: public BarrierSet {
private:

  static inline oop get_shenandoah_forwardee_helper(oop p) {
    assert(UseShenandoahGC, "must only be called when Shenandoah is used.");
    assert(Universe::heap()->is_in(p), "We shouldn't be calling this on objects not in the heap");
    oop forwardee;
#ifdef ASSERT
    if (ShenandoahTraceWritesToFromSpace) {
      ShenandoahHeapRegion* region = ShenandoahHeap::heap()->heap_region_containing(p);
      {
        region->memProtectionOff();
        forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
        region->memProtectionOn();
      }
    } else {
      forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
    }
#else
    forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
#endif
    return forwardee;
  }

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

  virtual oop resolve_oop(oop src);

  static inline oop resolve_oop_static_not_null(oop p) {
    assert(p != NULL, "Must be NULL checked");

    oop result = get_shenandoah_forwardee_helper(p);

#ifdef ASSERT
    if (result != p) {
        oop second_forwarding = get_shenandoah_forwardee_helper(result);

        // We should never be forwarded more than once.
        if (result != second_forwarding) {
          ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
          tty->print("first reference %p is in heap region:\n", (HeapWord*) p);
          sh->heap_region_containing(p)->print();
          tty->print("first_forwarding %p is in heap region:\n", (HeapWord*) result);
          sh->heap_region_containing(result)->print();
          tty->print("final reference %p is in heap region:\n", (HeapWord*) second_forwarding);
          sh->heap_region_containing(second_forwarding)->print();
          assert(get_shenandoah_forwardee_helper(result) == result, "Only one fowarding per customer");
        }
    }
#endif
    if (! ShenandoahTraceWritesToFromSpace) {
      // is_oop() would trigger a SEGFAULT when we're checking from-space-access.
      assert(ShenandoahHeap::heap()->is_in(result) && result->is_oop(), "resolved oop must be a valid oop in the heap");
    }
    return result;
  }

  static inline oopDesc* resolve_oop_static(oopDesc* p) {
    if (((HeapWord*) p) != NULL) {
      return resolve_oop_static_not_null(p);
    } else {
      return p;
    }
  }

  virtual oop maybe_resolve_oop(oop src);
  oop resolve_and_maybe_copy_oopHelper(oop src);
  oop resolve_and_maybe_copy_oop_work(oop src);
  oop resolve_and_maybe_copy_oop_work2(oop src);
  virtual oop resolve_and_maybe_copy_oop(oop src);
  static oopDesc* resolve_and_maybe_copy_oop_static(oopDesc* src);
  static oopDesc* resolve_and_maybe_copy_oop_static2(oopDesc* src);

private:
  bool need_update_refs_barrier();

#ifndef CC_INTERP
public:
  // TODO: The following should really live in an X86 specific subclass.
  virtual void compile_resolve_oop(MacroAssembler* masm, Register dst);
  virtual void compile_resolve_oop_not_null(MacroAssembler* masm, Register dst);
  void compile_resolve_oop_for_write(MacroAssembler* masm, Register dst, bool explicit_null_check, int num_save_state = 0, ...);

private:
  void compile_resolve_oop_runtime(MacroAssembler* masm, Register dst);

#endif
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

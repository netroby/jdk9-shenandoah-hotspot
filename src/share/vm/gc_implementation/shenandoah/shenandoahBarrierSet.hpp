#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "memory/barrierSet.hpp"

class ShenandoahBarrierSet: public BarrierSet {
private:
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

<<<<<<< local
  template <class T> static void write_ref_field_pre_static(T* field, oop newVal) {
    T heap_oop = oopDesc::load_heap_oop(field);
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
    if (sh->is_in(field) && 
	sh->heap_region_containing((HeapWord*)field)->is_in_collection_set()){
      tty->print("field = %p\n", field);
      sh->heap_region_containing((HeapWord*)field)->print();
      assert(false, "We should have fixed this earlier");   
    }   
  
    if (!oopDesc::is_null(heap_oop)) {
	G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop(heap_oop));
      // tty->print("write_ref_field_pre_static: v = "PTR_FORMAT" o = "PTR_FORMAT" old: %p\n", field, newVal, heap_oop);
    }
  }

  // We export this to make it available in cases where the static
  // type of the barrier set is known.  Note that it is non-virtual.
  template <class T> inline void inline_write_ref_field_pre(T* field, oop newVal) {
    write_ref_field_pre_static(field, newVal);
    
  }
=======
  template <class T> static void write_ref_field_pre_static(T* field, oop newVal);
>>>>>>> other

  // These are the more general virtual versions.
  void write_ref_field_pre_work(oop* field, oop new_val);
  void write_ref_field_pre_work(narrowOop* field, oop new_val);
  void write_ref_field_pre_work(void* field, oop new_val);

<<<<<<< local

  void write_ref_field_work(void* v, oop o){
    if (!JavaThread::satb_mark_queue_set().is_active()) return;
    assert (! UseCompressedOops, "compressed oops not supported yet");
    enqueue_update_ref((oop*) v);
    
    // tty->print("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT"\n", v, o);
  }

=======
  void write_ref_field_work(void* v, oop o);
>>>>>>> other
  void write_region_work(MemRegion mr);

  oopDesc* get_shenandoah_forwardee(oopDesc* p);

  static bool is_brooks_ptr(oopDesc* p);

  static bool has_brooks_ptr(oopDesc* p);

<<<<<<< local
  inline oopDesc* get_shenandoah_forwardee(oopDesc* p) {
    oop result = get_shenandoah_forwardee_helper(p);

    if (result != p) {
      oop second_forwarding = get_shenandoah_forwardee_helper(result);

      // We should never be forwarded more than once.
      if (result != second_forwarding) {
	ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
	tty->print("first reference %p is in heap region:\n", p);
	sh->heap_region_containing(p)->print();
	tty->print("first_forwarding %p is in heap region:\n", result);
	sh->heap_region_containing(result)->print();
	tty->print("final reference %p is in heap region:\n", second_forwarding);
	sh->heap_region_containing(second_forwarding)->print();
	assert(get_shenandoah_forwardee_helper(result) == result, "Only one fowarding per customer");  
      }
    }
    return result;
  }
=======
  virtual oopDesc* resolve_oop(oopDesc* src);
>>>>>>> other

<<<<<<< local
  static bool is_brooks_ptr(oopDesc* p) {
    if (p->has_displaced_mark())
      return false;

    return p->mark()->age()==15;
  }

  static bool has_brooks_ptr(oopDesc* p) {
    return is_brooks_ptr(oop(((HeapWord*) p) - BROOKS_POINTER_OBJ_SIZE));
  }

  virtual oopDesc* resolve_oop(oopDesc* src) {

    if (src != NULL) {
      return get_shenandoah_forwardee(src);
    } else {
      return NULL;
    }
  }

  virtual oopDesc* maybe_resolve_oop(oopDesc* src) {
    if (Universe::heap()->is_in(src)) {
      return get_shenandoah_forwardee(src);
    } else {
      return src;
    }
  }
=======
  virtual oopDesc* maybe_resolve_oop(oopDesc* src);
>>>>>>> other


  virtual oopDesc* resolve_and_maybe_copy_oopHelper(oopDesc* src) {
    if (src != NULL) {
      oopDesc* tmp = get_shenandoah_forwardee(src);
      ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();      
      if (sh->heap_region_containing(tmp)->is_in_collection_set()) {
	oopDesc* dst = sh->evacuate_object(tmp);
	tty->print("src = %p dst = %p tmp = %p src-2 = %p\n",
		   src, dst, tmp, src-2);
	assert(sh->is_in(dst), "result should be in the heap");
	return dst;
      } else {
	return src;
      }
    } else {
      return NULL;
    }
  }

  virtual oopDesc* resolve_and_maybe_copy_oop(oopDesc* src) {
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();      
    if (src != NULL && sh->is_in(src)) {
      oopDesc* result = resolve_and_maybe_copy_oopHelper(src);
      assert(sh->is_in(result), "result should be in the heap");
      return result;
    } else {
      return src;
    }
  }

  void enqueue_update_ref(oop* ref);

#ifndef CC_INTERP
  // TODO: The following should really live in an X86 specific subclass.
  virtual void compile_resolve_oop(MacroAssembler* masm, Register dst);
  virtual void compile_resolve_oop_not_null(MacroAssembler* masm, Register dst);
#endif
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

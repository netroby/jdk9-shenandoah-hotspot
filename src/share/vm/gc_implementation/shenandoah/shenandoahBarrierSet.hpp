#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "memory/barrierSet.hpp"
#include "memory/universe.hpp"
#include "gc_implementation/shenandoah/shenandoahSATBQueue.hpp"

#define BROOKS_POINTER_OBJ_SIZE 4

#define __ masm->

class ShenandoahBarrierSet: public BarrierSet {
  
  bool is_a(BarrierSet::Name bsn) {
    return bsn == BarrierSet::ShenandoahBarrierSet;
  }

  void print_on(outputStream* st) const {
    st->print("ShenandoahBarrierSet");
  }

public:

  ShenandoahBarrierSet() {
    _kind = BarrierSet::ShenandoahBarrierSet;
  }

  bool has_read_prim_array_opt() {return true;}
  bool has_read_prim_barrier() { return false;}
  bool has_read_ref_array_opt() {return true;}
  bool has_read_ref_barrier() {return false;}
  bool has_read_region_opt(){return true;}
  bool has_write_prim_array_opt(){return true;}
  bool has_write_prim_barrier() {return false;}
  bool has_write_ref_array_opt(){return true;}
  bool has_write_ref_barrier() { return true;}
  bool has_write_ref_pre_barrier() {return true;}
  bool has_write_region_opt(){return true;}
  bool is_aligned(HeapWord* hw) {return true;}
  void read_prim_array(MemRegion mr) {nyi();}
  void read_prim_field(HeapWord* hw, size_t s){nyi();}
  bool read_prim_needs_barrier(HeapWord* hw, size_t s) {return false;}
  void read_ref_array(MemRegion mr) {nyi();}

  void read_ref_field(void* v) {
    //    tty->print("read_ref_field: v = "PTR_FORMAT"\n", v);
    // return *v;
  }

  bool read_ref_needs_barrier(void* v) {nyi();}
  void read_region(MemRegion mr){nyi();}
  void resize_covered_region(MemRegion mr){nyi();}
  void write_prim_array(MemRegion mr){nyi();}
  void write_prim_field(HeapWord* hw, size_t s , juint x, juint y) {
    nyi();
  }
  bool write_prim_needs_barrier(HeapWord* hw, size_t s, juint x, juint y){
    nyi();
  }
  void write_ref_array_work(MemRegion mr){
  }

  template <class T> void
  write_ref_array_pre_work(T* dst, int count) {
    if (!JavaThread::satb_mark_queue_set()->is_active()) return;
    // tty->print_cr("write_ref_array_pre_work: %p, %d", dst, count);
    T* elem_ptr = dst;
    for (int i = 0; i < count; i++, elem_ptr++) {
      T heap_oop = oopDesc::load_heap_oop(elem_ptr);
      enqueue(heap_oop, elem_ptr);
      // tty->print("write_ref_array_pre_work: oop: "PTR_FORMAT"\n", heap_oop);
    }
  }

  virtual void write_ref_array_pre(oop* dst, int count, bool dest_uninitialized) {
    if (!dest_uninitialized) {
      write_ref_array_pre_work(dst, count);
    }
  }

  virtual void write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
    if (!dest_uninitialized) {
      write_ref_array_pre_work(dst, count);
    }
  }

  template <class T> static void write_ref_field_pre_static(T* field, oop newVal) {
    T heap_oop = oopDesc::load_heap_oop(field);
    enqueue(heap_oop, field);
    // tty->print("write_ref_field_pre_work: v = "PTR_FORMAT" o = "PTR_FORMAT" old: %p\n",
    //           field, newVal, heap_oop);
  }

  // We export this to make it available in cases where the static
  // type of the barrier set is known.  Note that it is non-virtual.
  template <class T> inline void inline_write_ref_field_pre(T* field, oop newVal) {
    write_ref_field_pre_static(field, newVal);
  }

  // These are the more general virtual versions.
  virtual void write_ref_field_pre_work(oop* field, oop new_val) {
    inline_write_ref_field_pre(field, new_val);
  }
  virtual void write_ref_field_pre_work(narrowOop* field, oop new_val) {
    inline_write_ref_field_pre(field, new_val);
  }
  virtual void write_ref_field_pre_work(void* field, oop new_val) {
    guarantee(false, "Not needed");
  }


  void write_ref_field_work(void* v, oop o){
    /*
    tty->print("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT"\n",
               v, o);
    */
  }

  void write_ref_field(void* v, oop o) {
    // tty->print("write_ref_field: v = "PTR_FORMAT" o = "PTR_FORMAT"\n",
    //	       v, o);
  }

  void write_region_work(MemRegion mr){
    // tty->print_cr("write_region_work: %p, %p", mr.start(), mr.end());
    // for (HeapWord* start  = mr.start(); start < mr.end(); start++) {
    //   tty->print_cr("write_region_work: oop (?): %p", *((oop*) start));
    // }
  }
  void nyi() {
    assert(false, "not yet implemented");
    tty->print_cr("Not yet implemented");
  }

  inline oopDesc* get_shenandoah_forwardee_helper(oopDesc* p) {
    assert(UseShenandoahGC, "must only be called when Shenandoah is used.");
    assert(Universe::heap()->is_in(p), "We shouldn't be calling this on objects not in the heap");
    assert(! is_brooks_ptr(p), "oop must not be a brooks pointer itself");
    HeapWord* oopWord = (HeapWord*) p;
    HeapWord* brooksPOop = oopWord - BROOKS_POINTER_OBJ_SIZE;
    if (!is_brooks_ptr(oop(brooksPOop))) {
      oopDesc* b = (oopDesc*) oop(brooksPOop);
      if (b->has_displaced_mark())
	tty->print("OOPSIE: displaced mark p = %p brooksPOop = %p mark = %p\n", oopWord, brooksPOop, b->mark());
      else
	tty->print("OOPSIE: oop = %p brooksPOop = %p age = %d   \n",
		   oopWord, brooksPOop, b->mark()->age());
    }
    assert(is_brooks_ptr(oop(brooksPOop)), err_msg("brooks pointer must be a brooks pointer %p", brooksPOop));
    HeapWord** brooksP = (HeapWord**) (brooksPOop + BROOKS_POINTER_OBJ_SIZE - 1);
    HeapWord* forwarded = *brooksP;
    return (oopDesc*) forwarded;
  }


  inline oopDesc* get_shenandoah_forwardee(oopDesc* p) {
    oop result = get_shenandoah_forwardee_helper(p);
    // We should never be forwarded more than once.
    assert(get_shenandoah_forwardee_helper(result) == result, "Only one fowarding per customer");  
    return result;
  }

  static bool is_brooks_ptr(oopDesc* p) {
    if (p->has_displaced_mark())
      return false;
    return p->mark()->age() == 15;
  }

  virtual oopDesc* resolve_oop(oopDesc* src) {
    if (src != NULL) {
      return get_shenandoah_forwardee(src);
    } else {
      return NULL;
    }
  }

private:
  static void enqueue(oop prev, oop* dst) {

    ShenandoahSATBElement* satb_elem = new ShenandoahSATBElement(prev, dst);
    
    if (!JavaThread::satb_mark_queue_set()->is_active()) return;
    Thread* thr = Thread::current();
    if (thr->is_Java_thread()) {
      JavaThread* jt = (JavaThread*)thr;
      jt->satb_mark_queue().enqueue(satb_elem);
    } else {
      MutexLocker x(Shared_SATB_Q_lock);
      ((ShenandoahSATBQueueSet*) JavaThread::satb_mark_queue_set())->shared_satb_queue()->enqueue(satb_elem);
    }
  }

  static void enqueue(narrowOop prev, narrowOop* dst) {
    Unimplemented(); // Maybe use templates above?
  }

#ifndef CC_INTERP
  // TODO: The following should really live in an X86 specific subclass.
  virtual void compile_resolve_oop(MacroAssembler* masm, Register dst) {
    Label is_null;
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, is_null);
    compile_resolve_oop_not_null(masm, dst);
    __ bind(is_null);
  }

  virtual void compile_resolve_oop_not_null(MacroAssembler* masm, Register dst) {
    __ movptr(dst, Address(dst, -8));
  }
#endif
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

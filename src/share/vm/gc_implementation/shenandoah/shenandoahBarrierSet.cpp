
#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "memory/universe.hpp"

#define __ masm->

class UpdateRefsForOopClosure: public ExtendedOopClosure {

private:
  ShenandoahBarrierSet* _bs;

public:
  UpdateRefsForOopClosure(ShenandoahBarrierSet* bs) : _bs(bs)
    { }

  void do_oop(oop* p)       {
    _bs->enqueue_update_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

ShenandoahBarrierSet::ShenandoahBarrierSet() {
  _kind = BarrierSet::ShenandoahBarrierSet;
}

void ShenandoahBarrierSet::print_on(outputStream* st) const {
  st->print("ShenandoahBarrierSet");
}

bool ShenandoahBarrierSet::is_a(BarrierSet::Name bsn) {
  return bsn == BarrierSet::ShenandoahBarrierSet;
}

bool ShenandoahBarrierSet::has_read_prim_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_read_prim_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_read_ref_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_read_ref_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_read_region_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_prim_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_prim_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_write_ref_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_ref_barrier() {
  return true;
}

bool ShenandoahBarrierSet::has_write_ref_pre_barrier() {
  return true;
}

bool ShenandoahBarrierSet::has_write_region_opt() {
  return true;
}

bool ShenandoahBarrierSet::is_aligned(HeapWord* hw) {
  return true;
}

void ShenandoahBarrierSet::read_prim_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::read_prim_field(HeapWord* hw, size_t s){
  Unimplemented();
}

bool ShenandoahBarrierSet::read_prim_needs_barrier(HeapWord* hw, size_t s) {
  return false;
}

void ShenandoahBarrierSet::read_ref_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::read_ref_field(void* v) {
  //    tty->print("read_ref_field: v = "PTR_FORMAT"\n", v);
  // return *v;
}

bool ShenandoahBarrierSet::read_ref_needs_barrier(void* v) {
  Unimplemented();
}

void ShenandoahBarrierSet::read_region(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::resize_covered_region(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::write_prim_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::write_prim_field(HeapWord* hw, size_t s , juint x, juint y) {
  Unimplemented();
}

bool ShenandoahBarrierSet::write_prim_needs_barrier(HeapWord* hw, size_t s, juint x, juint y) {
  Unimplemented();
}

void ShenandoahBarrierSet::write_ref_array_work(MemRegion mr) {
  if (!JavaThread::satb_mark_queue_set().is_active()) return;
  for (HeapWord* word = mr.start(); word < mr.end(); word++) {
    oop* oop_ptr = (oop*) word;
    enqueue_update_ref(oop_ptr);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, int count) {
  if (! JavaThread::satb_mark_queue_set().is_active()) return;
  // tty->print_cr("write_ref_array_pre_work: %p, %d", dst, count);
  T* elem_ptr = dst;
  for (int i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = oopDesc::load_heap_oop(elem_ptr);
    if (!oopDesc::is_null(heap_oop)) {
      G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop_not_null(heap_oop));
    }
    // tty->print("write_ref_array_pre_work: oop: "PTR_FORMAT"\n", heap_oop);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(oop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_field_pre_static(T* field, oop newVal) {
  T heap_oop = oopDesc::load_heap_oop(field);
  if (!oopDesc::is_null(heap_oop)) {
    G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop(heap_oop));
    // tty->print("write_ref_field_pre_static: v = "PTR_FORMAT" o = "PTR_FORMAT" old: %p\n", field, newVal, heap_oop);
  }
}

// These are the more general virtual versions.
void ShenandoahBarrierSet::write_ref_field_pre_work(oop* field, oop new_val) {
  write_ref_field_pre_static(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(narrowOop* field, oop new_val) {
  write_ref_field_pre_static(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(void* field, oop new_val) {
  guarantee(false, "Not needed");
}

void ShenandoahBarrierSet::write_ref_field_work(void* v, oop o) {
  if (! JavaThread::satb_mark_queue_set().is_active()) return;
  assert (! UseCompressedOops, "compressed oops not supported yet");
  enqueue_update_ref((oop*) v);
  // tty->print("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT"\n", v, o);
}

void ShenandoahBarrierSet::enqueue_update_ref(oop* ref) {

  if (!JavaThread::update_refs_queue_set().is_active()) return;
  // tty->print_cr("enqueueing update-ref: %p", ref);
  Thread* thr = Thread::current();
  if (thr->is_Java_thread()) {
    JavaThread* jt = (JavaThread*)thr;
    jt->update_refs_queue().enqueue((oop) ref);
  } else {
    MutexLocker x(Shared_SATB_Q_lock);
    JavaThread::update_refs_queue_set().shared_oop_queue()->enqueue((oop) ref);
  }
}


void ShenandoahBarrierSet::write_region_work(MemRegion mr) {

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  // tty->print_cr("write_region_work: %p, %p", mr.start(), mr.end());
  oop obj = oop(mr.start());
  assert(obj->is_oop(), "must be an oop");
  assert(ShenandoahBarrierSet::has_brooks_ptr(obj), "must have brooks pointer");
  UpdateRefsForOopClosure cl(this);
  obj->oop_iterate(&cl);
}

oopDesc* ShenandoahBarrierSet::get_shenandoah_forwardee_helper(oopDesc* p) {
  assert(UseShenandoahGC, "must only be called when Shenandoah is used.");
  assert(Universe::heap()->is_in(p), "We shouldn't be calling this on objects not in the heap");
  assert(! is_brooks_ptr(p), err_msg("oop must not be a brooks pointer itself. oop's mark word: %p", BrooksPointer::get(p).get_age()));
  return BrooksPointer::get(p).get_forwardee();
}


oopDesc* ShenandoahBarrierSet::get_shenandoah_forwardee(oopDesc* p) {
  oop result = get_shenandoah_forwardee_helper(p);
  // We should never be forwarded more than once.
  assert(get_shenandoah_forwardee_helper(result) == result, "Only one fowarding per customer");  
  return result;
}


bool ShenandoahBarrierSet::is_brooks_ptr(oopDesc* p) {
  markOop mark = p->mark();
  if (mark->has_displaced_mark_helper()) {
    return false;
  } else {
    return mark->age() == 15;
  }
}

bool ShenandoahBarrierSet::has_brooks_ptr(oopDesc* p) {
  return is_brooks_ptr(oop(((HeapWord*) p) - BROOKS_POINTER_OBJ_SIZE));
}

oopDesc* ShenandoahBarrierSet::resolve_oop(oopDesc* src) {

  if (src != NULL) {
    return get_shenandoah_forwardee(src);
  } else {
    return NULL;
  }
}

oopDesc* ShenandoahBarrierSet::maybe_resolve_oop(oopDesc* src) {
  if (Universe::heap()->is_in(src)) {
    return get_shenandoah_forwardee(src);
  } else {
    return src;
  }
}


#ifndef CC_INTERP
// TODO: The following should really live in an X86 specific subclass.
void ShenandoahBarrierSet::compile_resolve_oop(MacroAssembler* masm, Register dst) {
  Label is_null;
  __ testptr(dst, dst);
  __ jcc(Assembler::zero, is_null);
  compile_resolve_oop_not_null(masm, dst);
  __ bind(is_null);
}

void compile_resolve_oop_not_null(MacroAssembler* masm, Register dst) {
  __ movptr(dst, Address(dst, -8));
  __ andq(dst, ~0x7);
}
#endif

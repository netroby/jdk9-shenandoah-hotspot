/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "memory/universe.hpp"
#include "utilities/array.hpp"

#define __ masm->

class UpdateRefsForOopClosure: public ExtendedOopClosure {

private:
  ShenandoahHeap* _heap;
public:
  UpdateRefsForOopClosure() {
    _heap = ShenandoahHeap::heap();
  }

  void do_oop(oop* p)       {
    _heap->maybe_update_oop_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

ShenandoahBarrierSet::ShenandoahBarrierSet() : BarrierSet(BarrierSet::ShenandoahBarrierSet) {
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
  //    tty->print_cr("read_ref_field: v = "PTR_FORMAT, v);
  // return *v;
}

bool ShenandoahBarrierSet::read_ref_needs_barrier(void* v) {
  Unimplemented();
  return false;
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
  return false;
}

bool ShenandoahBarrierSet::need_update_refs_barrier() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  return ShenandoahUpdateRefsEarly ? heap->is_update_references_in_progress()
                                   : JavaThread::satb_mark_queue_set().is_active();
}

void ShenandoahBarrierSet::write_ref_array_work(MemRegion mr) {
  if (! need_update_refs_barrier()) return;
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (HeapWord* word = mr.start(); word < mr.end(); word++) {
    oop* oop_ptr = (oop*) word;
    heap->maybe_update_oop_ref(oop_ptr);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, int count) {

#ifdef ASSERT
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
    if (sh->is_in(dst) && 
	sh->heap_region_containing((HeapWord*) dst)->is_in_collection_set() &&
        ! sh->cancelled_evacuation()) {
      tty->print_cr("dst = "PTR_FORMAT, p2i(dst));
      sh->heap_region_containing((HeapWord*) dst)->print();
      assert(false, "We should have fixed this earlier");   
    }   
#endif

  if (! JavaThread::satb_mark_queue_set().is_active()) return;
  // tty->print_cr("write_ref_array_pre_work: "PTR_FORMAT", "INT32_FORMAT, dst, count);
  T* elem_ptr = dst;
  for (int i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = oopDesc::load_heap_oop(elem_ptr);
    if (!oopDesc::is_null(heap_oop)) {
      G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop_not_null(heap_oop));
    }
    // tty->print_cr("write_ref_array_pre_work: oop: "PTR_FORMAT, heap_oop);
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

#ifdef ASSERT
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
    if (sh->is_in(field) && 
	sh->heap_region_containing((HeapWord*)field)->is_in_collection_set() &&
        ! sh->cancelled_evacuation()) {
      tty->print_cr("field = "PTR_FORMAT, p2i(field));
      sh->heap_region_containing((HeapWord*)field)->print();
      assert(false, "We should have fixed this earlier");   
    }   
#endif

  if (!oopDesc::is_null(heap_oop)) {
    G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop(heap_oop));
    // tty->print_cr("write_ref_field_pre_static: v = "PTR_FORMAT" o = "PTR_FORMAT" old: "PTR_FORMAT, field, newVal, heap_oop);
  }
}

template <class T>
inline void ShenandoahBarrierSet::inline_write_ref_field_pre(T* field, oop newVal) {
  write_ref_field_pre_static(field, newVal);
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

void ShenandoahBarrierSet::write_ref_field_work(void* v, oop o, bool release) {
  if (! need_update_refs_barrier()) return;
  assert (! UseCompressedOops, "compressed oops not supported yet");
  ShenandoahHeap::heap()->maybe_update_oop_ref((oop*) v);
  // tty->print_cr("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT, v, o);
}

void ShenandoahBarrierSet::write_region_work(MemRegion mr) {

  if (! need_update_refs_barrier()) return;

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  // tty->print_cr("write_region_work: "PTR_FORMAT", "PTR_FORMAT, mr.start(), mr.end());
  oop obj = oop(mr.start());
  assert(obj->is_oop(), "must be an oop");
  UpdateRefsForOopClosure cl;
  obj->oop_iterate(&cl);
}

oop ShenandoahBarrierSet::resolve_oop(oop src) {
  return ShenandoahBarrierSet::resolve_oop_static(src);
}

oop ShenandoahBarrierSet::maybe_resolve_oop(oop src) {
  if (Universe::heap()->is_in(src)) {
    return resolve_oop_static(src);
  } else {
    return src;
  }
}

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oop_work(oop src) {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  assert(src != NULL, "only evacuated non NULL oops");

  if (sh->in_cset_fast_test((HeapWord*) src)) {
    return resolve_and_maybe_copy_oop_work2(src);
  } else {
    return src;
  }
}

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oop_work2(oop src) {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  if (! sh->is_evacuation_in_progress()) {
    // We may get here through a barrier that just took a safepoint that
    // turned off evacuation. In this case, return right away.
    return ShenandoahBarrierSet::resolve_oop_static(src);
  }
  assert(src != NULL, "only evacuated non NULL oops");
  assert(sh->heap_region_containing(src)->is_in_collection_set(), "only evacuate objects in collection set");
  assert(! sh->heap_region_containing(src)->is_humonguous(), "never evacuate humonguous objects");
  // TODO: Consider passing thread from caller.
  oop dst = sh->evacuate_object(src, Thread::current());
#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print_cr("src = "PTR_FORMAT" dst = "PTR_FORMAT" src = "PTR_FORMAT" src-2 = "PTR_FORMAT,
                 p2i((HeapWord*) src), p2i((HeapWord*) dst), p2i((HeapWord*) src), p2i(((HeapWord*) src) - 2));
    }
#endif
  assert(sh->is_in(dst), "result should be in the heap");
  return dst;
}

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oopHelper(oop src) {
    if (src != NULL) {
      ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
      oop tmp = resolve_oop_static(src);
      if (! sh->is_evacuation_in_progress()) {
        return tmp;
      }
      return resolve_and_maybe_copy_oop_work(src);
    } else {
      return NULL;
    }
}

JRT_ENTRY(void, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_c2(oopDesc* src, JavaThread* thread))
  oop result = ((ShenandoahBarrierSet*) oopDesc::bs())->resolve_and_maybe_copy_oop_work(oop(src));
  // tty->print_cr("called C2 write barrier with: %p result: %p copy: %d", (oopDesc*) src, (oopDesc*) result, src != result);
  thread->set_vm_result(result);
  // eturn (oopDesc*) result;
JRT_END

IRT_ENTRY(void, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_interp(JavaThread* thread, oopDesc* src))
  oop result = ((ShenandoahBarrierSet*)oopDesc::bs())->resolve_and_maybe_copy_oop_work2(oop(src));
  // tty->print_cr("called interpreter write barrier with: %p result: %p", src, result);
  thread->set_vm_result(result);
  //return (oopDesc*) result;
IRT_END

JRT_ENTRY(void, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_c1(JavaThread* thread, oopDesc* src))
  oop result = ((ShenandoahBarrierSet*)oopDesc::bs())->resolve_and_maybe_copy_oop_work2(oop(src));
  // tty->print_cr("called static write barrier (2) with: "PTR_FORMAT" result: "PTR_FORMAT, p2i(src), p2i((oopDesc*)(result)));
  thread->set_vm_result(result);
JRT_END

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oop(oop src) {
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();      
    oop result;
    if (src != NULL && sh->is_in(src)) {
      result = resolve_and_maybe_copy_oopHelper(src);
      assert(sh->is_in(result), "result should be in the heap");
    } else {
      result = src;
    }
    assert(result == NULL || (sh->is_in(result) && result->is_oop()), "resolved oop must be NULL, or a valid oop in the heap");
    return result;
  }

#ifndef CC_INTERP
void ShenandoahBarrierSet::compile_resolve_oop_runtime(MacroAssembler* masm, Register dst) {

  __ push(rscratch1);

  if (dst != rax) {
    __ push(rax);
  }
  if (dst != rbx) {
    __ push(rbx);
  }
  if (dst != rcx) {
    __ push(rcx);
  }
  if (dst != rdx) {
    __ push(rdx);
  }
  if (dst != rdi) {
    __ push(rdi);
  }
  if (dst != rsi) {
    __ push(rsi);
  }
  if (dst != rbp) {
    __ push(rbp);
  }
  if (dst != r8) {
    __ push(r8);
  }
  if (dst != r9) {
    __ push(r9);
  }
  if (dst != r11) {
    __ push(r11);
  }
  if (dst != r12) {
    __ push(r12);
  }
  if (dst != r13) {
    __ push(r13);
  }
  if (dst != r14) {
    __ push(r14);
  }
  if (dst != r15) {
    __ push(r15);
  }

  __ subptr(rsp, 128);
  __ movdbl(Address(rsp, 0), xmm0);
  __ movdbl(Address(rsp, 8), xmm1);
  __ movdbl(Address(rsp, 16), xmm2);
  __ movdbl(Address(rsp, 24), xmm3);
  __ movdbl(Address(rsp, 32), xmm4);
  __ movdbl(Address(rsp, 40), xmm5);
  __ movdbl(Address(rsp, 48), xmm6);
  __ movdbl(Address(rsp, 56), xmm7);
  __ movdbl(Address(rsp, 64), xmm8);
  __ movdbl(Address(rsp, 72), xmm9);
  __ movdbl(Address(rsp, 80), xmm10);
  __ movdbl(Address(rsp, 88), xmm11);
  __ movdbl(Address(rsp, 96), xmm12);
  __ movdbl(Address(rsp, 104), xmm13);
  __ movdbl(Address(rsp, 112), xmm14);
  __ movdbl(Address(rsp, 120), xmm15);

  __ mov(c_rarg1, dst);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::resolve_oop_static), c_rarg1);
  __ mov(rscratch1, rax);

  __ movdbl(xmm0, Address(rsp, 0));
  __ movdbl(xmm1, Address(rsp, 8));
  __ movdbl(xmm2, Address(rsp, 16));
  __ movdbl(xmm3, Address(rsp, 24));
  __ movdbl(xmm4, Address(rsp, 32));
  __ movdbl(xmm5, Address(rsp, 40));
  __ movdbl(xmm6, Address(rsp, 48));
  __ movdbl(xmm7, Address(rsp, 56));
  __ movdbl(xmm8, Address(rsp, 64));
  __ movdbl(xmm9, Address(rsp, 72));
  __ movdbl(xmm10, Address(rsp, 80));
  __ movdbl(xmm11, Address(rsp, 88));
  __ movdbl(xmm12, Address(rsp, 96));
  __ movdbl(xmm13, Address(rsp, 104));
  __ movdbl(xmm14, Address(rsp, 112));
  __ movdbl(xmm15, Address(rsp, 120));
  __ addptr(rsp, 128);

  if (dst != r15) {
    __ pop(r15);
  }
  if (dst != r14) {
    __ pop(r14);
  }
  if (dst != r13) {
    __ pop(r13);
  }
  if (dst != r12) {
    __ pop(r12);
  }
  if (dst != r11) {
    __ pop(r11);
  }
  if (dst != r9) {
    __ pop(r9);
  }
  if (dst != r8) {
    __ pop(r8);
  }
  if (dst != rbp) {
    __ pop(rbp);
  }
  if (dst != rsi) {
    __ pop(rsi);
  }
  if (dst != rdi) {
    __ pop(rdi);
  }
  if (dst != rdx) {
    __ pop(rdx);
  }
  if (dst != rcx) {
    __ pop(rcx);
  }
  if (dst != rbx) {
    __ pop(rbx);
  }
  if (dst != rax) {
    __ pop(rax);
  }

  __ mov(dst, rscratch1);

  __ pop(rscratch1);
}

// TODO: The following should really live in an X86 specific subclass.
void ShenandoahBarrierSet::compile_resolve_oop(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {

    Label is_null;
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, is_null);
    compile_resolve_oop_not_null(masm, dst);
    __ bind(is_null);
  }
}

void ShenandoahBarrierSet::compile_resolve_oop_not_null(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    if (ShenandoahVerifyReadsToFromSpace) {
      compile_resolve_oop_runtime(masm, dst);
      return;
    }
    __ movptr(dst, Address(dst, -8));
  }
}

void ShenandoahBarrierSet::compile_resolve_oop_for_write(MacroAssembler* masm, Register dst, bool explicit_null_check, int stack_adjust, int num_state_save, ...) {

  if (! ShenandoahWriteBarrier) {
    assert(! ShenandoahConcurrentEvacuation, "Can only do this without concurrent evacuation");
    return compile_resolve_oop(masm, dst);
  }
      
  assert(dst != rscratch1, "different regs");
  //assert(dst != rscratch2, "Need rscratch2");

  Label done;

  // Resolve oop first.
  // TODO: Make this not-null-checking as soon as we have implicit null checks in c1!

  if (explicit_null_check) {
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, done);
  }

  // Now check if evacuation is in progress.
  compile_resolve_oop_not_null(masm, dst);

  Address evacuation_in_progress = Address(r15_thread, in_bytes(JavaThread::evacuation_in_progress_offset()));

  __ cmpb(evacuation_in_progress, 0);
  __ jcc(Assembler::equal, done);
  __ push(rscratch1);
  __ push(rscratch2);

  __ movptr(rscratch1, dst);
  __ shrptr(rscratch1, ShenandoahHeapRegion::RegionSizeShift);
  __ movptr(rscratch2, (intptr_t) ShenandoahHeap::in_cset_fast_test_addr());
  __ movbool(rscratch2, Address(rscratch2, rscratch1, Address::times_1));
  __ testb(rscratch2, 0x1);

  __ pop(rscratch2);
  __ pop(rscratch1);

  __ jcc(Assembler::zero, done);

  intArray save_states = intArray(num_state_save);
  va_list vl;
  va_start(vl, num_state_save);
  for (int i = 0; i < num_state_save; i++) {
    save_states.at_put(i, va_arg(vl, int));
  }
  va_end(vl);

  if (stack_adjust != 0) {
    __ addptr(rsp, stack_adjust * Interpreter::stackElementSize);
  }
  for (int i = 0; i < num_state_save; i++) {
    switch (save_states[i]) {
    case noreg:
      __ subptr(rsp, Interpreter::stackElementSize);
      break;
    case ss_rax:
      __ push(rax);
      break;
    case ss_rbx:
      __ push(rbx);
      break;
    case ss_rcx:
      __ push(rcx);
      break;
    case ss_rdx:
      __ push(rdx);
      break;
    case ss_rsi:
      __ push(rsi);
      break;
    case ss_rdi:
      __ push(rdi);
      break;
    case ss_r13:
      __ push(r13);
      break;
    case ss_ftos:
      __ subptr(rsp, wordSize);
      __ movflt(Address(rsp, 0), xmm0);
      break;
    case ss_dtos:
      __ subptr(rsp, 2 * wordSize);
      __ movdbl(Address(rsp, 0), xmm0);
      break;
    case ss_c_rarg0:
      __ push(c_rarg0);
      break;
    case ss_c_rarg1:
      __ push(c_rarg1);
      break;
    case ss_c_rarg2:
      __ push(c_rarg2);
      break;
    case ss_c_rarg3:
      __ push(c_rarg3);
      break;
    case ss_c_rarg4:
      __ push(c_rarg4);
      break;

    default:
      ShouldNotReachHere();
    }
  }

  __ call_VM(dst, CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_interp), dst, false);

  for (int i = num_state_save - 1; i >= 0; i--) {
    switch (save_states[i]) {
    case noreg:
      __ addptr(rsp, Interpreter::stackElementSize);
      break;
    case ss_rax:
      __ pop(rax);
      break;
    case ss_rbx:
      __ pop(rbx);
      break;
    case ss_rcx:
      __ pop(rcx);
      break;
    case ss_rdx:
      __ pop(rdx);
      break;
    case ss_rsi:
      __ pop(rsi);
      break;
    case ss_rdi:
      __ pop(rdi);
      break;
    case ss_r13:
      __ pop(r13);
      break;
    case ss_ftos:
      __ movflt(xmm0, Address(rsp, 0));
      __ addptr(rsp, wordSize);
      break;
    case ss_dtos:
      __ movdbl(xmm0, Address(rsp, 0));
      __ addptr(rsp, 2 * Interpreter::stackElementSize);
      break;
    case ss_c_rarg0:
      __ pop(c_rarg0);
      break;
    case ss_c_rarg1:
      __ pop(c_rarg1);
      break;
    case ss_c_rarg2:
      __ pop(c_rarg2);
      break;
    case ss_c_rarg3:
      __ pop(c_rarg3);
      break;
    case ss_c_rarg4:
      __ pop(c_rarg4);
      break;
    default:
      ShouldNotReachHere();
    }
  }

  if (stack_adjust != 0) {
    __ subptr(rsp, stack_adjust * Interpreter::stackElementSize);
  }
  __ bind(done);
}

/*
void ShenandoahBarrierSet::compile_resolve_oop_for_write(MacroAssembler* masm, Register dst) {

  Label is_null;
  __ testptr(dst, dst);
  __ jcc(Assembler::zero, is_null);
  compile_resolve_oop_for_write_not_null(masm, dst);
  __ bind(is_null);

}
*/
#endif


#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacuation.hpp"
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

ShenandoahBarrierSet::ShenandoahBarrierSet() {
  _kind = BarrierSet::ShenandoahBarrierSet;
  _allocator = TLABAllocator();
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
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (HeapWord* word = mr.start(); word < mr.end(); word++) {
    oop* oop_ptr = (oop*) word;
    heap->maybe_update_oop_ref(oop_ptr);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, int count) {
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
    if (sh->is_in(dst) && 
	sh->heap_region_containing((HeapWord*) dst)->is_in_collection_set()){
      tty->print("dst = %p\n", dst);
      sh->heap_region_containing((HeapWord*) dst)->print();
      assert(false, "We should have fixed this earlier");   
    }   

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
  ShenandoahHeap::heap()->maybe_update_oop_ref((oop*) v);
  // tty->print("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT"\n", v, o);
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
  UpdateRefsForOopClosure cl;
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
    oopDesc* result = get_shenandoah_forwardee(src);
    assert(ShenandoahHeap::heap()->is_in(result) && result->is_oop(), "resolved oop must be a valid oop in the heap");
    return result;
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

oopDesc* ShenandoahBarrierSet::resolve_and_maybe_copy_oopHelper(oopDesc* src) {
    if (src != NULL) {
      oopDesc* tmp = get_shenandoah_forwardee(src);
      ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();      
      if (sh->heap_region_containing(tmp)->is_in_collection_set()) {
	oopDesc* dst = sh->evacuate_object(tmp, &_allocator);
#ifdef ASSERT
        if (ShenandoahGCVerbose) {
          tty->print("src = %p dst = %p tmp = %p src-2 = %p\n",
                     src, dst, tmp, src-2);
        }
#endif
	assert(sh->is_in(dst), "result should be in the heap");
	return dst;
      } else {
	return tmp;
      }
    } else {
      return NULL;
    }
}

IRT_LEAF(oopDesc*, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_static(oopDesc* src))
  oop result = oopDesc::bs()->resolve_and_maybe_copy_oop(src);
  // tty->print_cr("called write barrier with: %p result: %p", src, result);
  return result;
IRT_END

oopDesc* ShenandoahBarrierSet::resolve_and_maybe_copy_oop(oopDesc* src) {
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();      
    oopDesc* result;
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
// TODO: The following should really live in an X86 specific subclass.
void ShenandoahBarrierSet::compile_resolve_oop(MacroAssembler* masm, Register dst) {
  Label is_null;
  __ testptr(dst, dst);
  __ jcc(Assembler::zero, is_null);
  compile_resolve_oop_not_null(masm, dst);
  __ bind(is_null);
}

void ShenandoahBarrierSet::compile_resolve_oop_not_null(MacroAssembler* masm, Register dst) {
  __ movptr(dst, Address(dst, -8));
  __ andq(dst, ~0x7);
}

void ShenandoahBarrierSet::compile_resolve_oop_for_write(MacroAssembler* masm, Register dst, int num_state_save, ...) {
  assert(dst != rscratch1, "Need rscratch1");
  assert(dst != rscratch2, "Need rscratch2");

  intArray save_states = intArray(num_state_save);
  va_list vl;
  va_start(vl, num_state_save);
  for (int i = 0; i < num_state_save; i++) {
    save_states.at_put(i, va_arg(vl, int /* SaveState */));
  }
  va_end(vl);

  for (int i = 0; i < num_state_save; i++) {
    switch (save_states[i]) {
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

  /*
  Label done;

  // Resolve oop.
  __ movptr(dst, Address(dst, -8));
  __ andq(dst, ~0x7);

  __ os_breakpoint();

  // Check if the heap region containing the oop is in the collection set.
  ExternalAddress heap_address = ExternalAddress((address) Universe::heap_addr());
  __ movptr(rscratch1, heap_address);

  // Compute index into regions array.
  __ movq(rscratch2, dst);
  __ andq(rscratch2, ~(ShenandoahHeapRegion::RegionSizeBytes - 1));
  Address first_region_bottom_addr = Address(rscratch1, ShenandoahHeap::first_region_bottom_offset());
  __ subq(rscratch2, first_region_bottom_addr);
  __ shrq(rscratch2, ShenandoahHeapRegion::RegionSizeShift);

  Address regions_address = Address(rscratch1, ShenandoahHeap::ordered_regions_offset());
  __ movptr(rscratch1, regions_address);

  Address heap_region_containing_addr = Address(rscratch1, rscratch2, Address::times_ptr);
  __ movptr(rscratch1, heap_region_containing_addr);

  Address is_in_coll_set_addr = Address(rscratch1, ShenandoahHeapRegion::is_in_collection_set_offset());

  __ movb(rscratch1, is_in_coll_set_addr);
  __ testb(rscratch1, 0x1);
  __ jcc(Assembler::zero, done);

  __ movptr(c_rarg1, dst);
  // __ push_callee_saved_registers();
  __ push(rsi);
  __ push(rdi);
  __ push(rdx);
  __ push(rcx);
  __ push(rbx);
  __ push(rax);

  __ call_VM(c_rarg1, CAST_FROM_FN_PTR(address, ShenandoahHeap::allocate_memory_static), c_rarg1);
  // __ pop_callee_saved_registers();
  __ pop(rax);
  __ pop(rbx);
  __ pop(rcx);
  __ pop(rdx);
  __ pop(rdi);
  __ pop(rsi);
  __ movptr(dst, c_rarg1);

  // __ stop("CAS not yet implemented");
  __ bind(done);
*/

  __ mov(c_rarg1, dst);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::resolve_and_maybe_copy_oop_static), c_rarg1);
  __ mov(rscratch1, rax);

  for (int i = num_state_save - 1; i >= 0; i--) {
    switch (save_states[i]) {
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

  __ mov(dst, rscratch1);
  __ os_breakpoint();

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

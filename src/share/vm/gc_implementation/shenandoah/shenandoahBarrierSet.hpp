#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "memory/barrierSet.hpp"
#include "memory/universe.hpp"

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
  void write_ref_array_work(MemRegion mr){}
  void write_ref_field_work(void* v, oop o){
    //    tty->print("write_ref_field_work: v = "PTR_FORMAT" o = "PTR_FORMAT"\n",
    //	       v, o);
  }
  void write_ref_field_pre(void* v, oop o){}

  void write_ref_field(void* v, oop o) {
    //    tty->print("write_ref_field: v = "PTR_FORMAT" o = "PTR_FORMAT"\n",
    //	       v, o);
  }

  void write_region_work(MemRegion mr){}
  void nyi() {
    assert(false, "not yet implemented");
    tty->print_cr("Not yet implemented");
  }
};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

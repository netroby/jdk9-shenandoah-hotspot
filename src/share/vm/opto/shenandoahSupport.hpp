#ifndef SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP
#define SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP

#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

class Node;
class ShenandoahBarrierNode;

class ShenandoahBarrierVisitor {
public:
  virtual void push_barrier(Node* b) = 0;
  virtual void pop_barrier(Node* b) {};
  virtual void leaf(bool need_barrier) {}

};

class ShenandoahMemoryVisitor {
public:
  virtual void leaf(bool polluted) {}

};

class ShenandoahSupport : AllStatic {
private:
  static void follow_through_phis(Node* phi, ShenandoahBarrierVisitor* v, GrowableArray<Node*>* phistack);
  static void visit_barrier_chain(Node* n, ShenandoahBarrierVisitor* v, GrowableArray<Node*>* phistack);
  static void visit_memory_chain_through_phi(Node* mem, int alias, ShenandoahMemoryVisitor* v, GrowableArray<Node*>* phistack);
  static void visit_memory_chain(Node* mem, int alias, ShenandoahMemoryVisitor* v, GrowableArray<Node*>* phistack);
 public:
  static Node* skip_through_barrier(Node* n);

  static bool can_eliminate_barrier(PhaseTransform* phase, const Type* t, Node* n);

  static void increase_read_used(PhaseIterGVN& igvn, Node* b);
  static void increase_write_used(PhaseIterGVN& igvn, Node* b);

  static void optimize_barrier(PhaseIterGVN& igvn, Node* b);
  static void eliminate_barrier(PhaseIterGVN& igvn, Node* b);
  static bool is_shenandoah_barrier(Node* n);
  static bool is_use_read(Node* n);
  static bool is_use_write(Node* n);
  static bool is_keep(Node* n);
  static void set_keep(Node* n, bool keep);
  static uint read_used(Node* n);
  static uint write_used(Node* n);
  static const TypePtr* slice(Node* n);
  static bool can_get_barrier_input(Node* n);
  static Node* barrier_input(Node* b);
};

class ShenandoahReadBarrierNode : public LoadPNode {
private:
  bool _is_use_read;
  bool _keep;
  const TypePtr* _slice;
  uint _read_used;
  uint _write_used;
public:
  ShenandoahReadBarrierNode(Compile* C, Node *c, Node *mem, Node *adr, const TypePtr *at, const TypePtr* t, MemOrd mo, const TypePtr* slice)
    : LoadPNode(c, mem, adr, at, t, mo), _is_use_read(false), _keep(false), _slice(slice), _read_used(0), _write_used(0) {

    init_class_id(Class_ShenandoahReadBarrier);
    init_flags(Flag_is_macro);
    C->add_macro_node(this);
  }

  void set_is_use_read(bool is_use_read) {
    _is_use_read = is_use_read;
  }
  bool is_use_read() {
    return _is_use_read;
  }
  void set_keep(bool keep) {
    _keep = keep;
  }
  bool is_keep() {
    return _keep;
  }
  const TypePtr* slice() {
    return _slice;
  }
  uint read_used() {
    return _read_used;
  }
  void increase_read_used() {
    _read_used++;
  }
  uint write_used() {
    return _write_used;
  }
  void increase_write_used() {
    _write_used++;
  }
  virtual int Opcode() const;

  virtual uint size_of() const { return sizeof(*this); }

  /*
  virtual const Type* Value( PhaseTransform* phase ) const {
    if (ShenandoahSupport::can_get_barrier_input((Node*) this)) {
      Node* in = ShenandoahSupport::barrier_input((Node*) this);
      const Type* input_type = phase->type(in);
      return input_type;
    } else {
      return LoadPNode::Value(phase);
    }
  }
  */
};

class ShenandoahBarrierNode : public CallNode {
private:
  uint _read_used;
  uint _write_used;
  bool _is_use_read;
  bool _is_use_write;
  bool _keep;
  const TypePtr* _slice;

public:
  static const TypeFunc* barrier_type(const Type* type) {
    // create input type (domain)
    const Type** fields = TypeTuple::fields(1);
    fields[TypeFunc::Parms+0] = type;

    const TypeTuple* domain = TypeTuple::make(TypeFunc::Parms+1, fields);

    // result type needed
    fields = TypeTuple::fields(1);
    fields[TypeFunc::Parms+0] = type;
    const TypeTuple* range = TypeTuple::make(TypeFunc::Parms + 1, fields);
    return TypeFunc::make(domain, range);
  }

  ShenandoahBarrierNode(Compile* C, const TypeFunc* tf, const TypePtr* adr_type, const TypePtr* slice)
    : CallNode(tf, NULL, adr_type), _slice(slice), _read_used(0), _write_used(0), _is_use_read(false), _is_use_write(false), _keep(false)
  {
    init_class_id(Class_ShenandoahBarrier);
    init_flags(Flag_is_macro);
    C->add_macro_node(this);
  }

  virtual uint size_of() const { return sizeof(*this); }

  virtual bool guaranteed_safepoint() { return false; }

  virtual bool        may_modify(const TypeOopPtr *t_oop, PhaseTransform *phase) { return false;}

  virtual int Opcode() const;

#ifndef PRODUCT
  virtual void dump_spec(outputStream *st) const {
    CallNode::dump_spec(st);
    st->print(" (read-used: %u write-used: %u)", _read_used, _write_used);
  }
#endif

  void increase_read_used() {
    _read_used++;
  }

  uint read_used() {
    return _read_used;
  }

  void increase_write_used() {
    _write_used++;
  }

  uint write_used() {
    return _write_used;
  }

  void set_is_use_write(bool is_use) {
    _is_use_write = is_use;
  }

  bool is_use_read() {
    return _is_use_read;
  }

  void set_is_use_read(bool is_use) {
    _is_use_read = is_use;
  }

  bool is_use_write() {
    return _is_use_write;
  }

  bool is_use() {
    return _is_use_read || _is_use_write;
  }

  bool is_keep() {
    return _keep;
  }
  void set_keep(bool keep) {
    _keep = keep;
  }
  const TypePtr* slice() {
    return _slice;
  }
};


#endif // SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP


#include "opto/addnode.hpp"
#include "opto/callnode.hpp"
#include "opto/cfgnode.hpp"
#include "opto/macro.hpp"
#include "opto/memnode.hpp"
#include "opto/node.hpp"
#include "opto/runtime.hpp"
#include "opto/shenandoahSupport.hpp"
#include "opto/subnode.hpp"

Node* ShenandoahSupport::skip_through_barrier(Node* n) {
  while (is_shenandoah_barrier(n)) {
    if (can_get_barrier_input(n)) {
      n = barrier_input(n);
    } else {
      break;
    }
  }
  return n;
}

bool ShenandoahSupport::can_eliminate_barrier(PhaseTransform* phase, const Type* t, Node* n) {

  if (t->higher_equal(TypePtr::NULL_PTR)) {
    //tty->print_cr("eliminate barrier because it's null");
    return true;
  }
  AllocateNode* alloc = AllocateNode::Ideal_allocation(n, phase);
  if (alloc != NULL) {
    return true;
  }
  if (t->is_oopptr()->const_oop() != NULL) {
    //tty->print_cr("eliminate barrier because it's constant");
    return true;
  }
  /*
  if (n->is_ShenandoahWriteBarrier()) {
    //tty->print_cr("eliminate barrier because it's dominated by write barrier");
    return true;
  }
  */
  return false;
}

Node* PhaseMacroExpand::make_basic_read_barrier(Node* n, Node* ctrl, Node* mem, const TypePtr* obj_type)
{
  Node* bp_addr = new AddPNode(n, n, longcon(-0x8));
  bp_addr = _igvn.transform(bp_addr);
  //const TypePtr* adr_type = TypeRawPtr::BOTTOM;
  const TypePtr* adr_type = bp_addr->bottom_type()->is_ptr();
  //tty->print("rb adr_type: "); adr_type->dump(); tty->print("\n");
  Node* bp_load = new LoadPNode(ctrl, mem, bp_addr, adr_type, obj_type, MemNode::unordered);
  bp_load = _igvn.transform(bp_load);
  return bp_load;
}

void PhaseMacroExpand::expand_read_barrier(Node* rb) {
  if (rb->Opcode() == Op_ShenandoahReadBarrier) {
    // Replace simple read barrier with actual LoadP.
    Node* ctrl = rb->in(MemNode::Control);
    Node* mem = rb->in(MemNode::Memory);
    Node* addr = rb->in(MemNode::Address);
    const TypePtr* adr_type = rb->as_Mem()->adr_type();
    const TypePtr* type = rb->as_Load()->type()->is_ptr();
    Node* ld = new LoadPNode(ctrl, mem, addr, adr_type, type, MemNode::unordered);
    ld = _igvn.transform(ld);
    _igvn.replace_node(rb, ld);
    return;
  }
  Node* n = rb->in(TypeFunc::Parms + 0);
  Node* ctrl = rb->in(TypeFunc::Control);
  Node* mem = rb->in(TypeFunc::Memory);

  //const TypePtr* obj_type = _igvn.type(n)->is_ptr();
  //if (!_igvn.type(rb)->isa_ptr()) _igvn.type(rb)->dump();
  const TypePtr* obj_type = _igvn.type(rb)->is_tuple()->field_at(TypeFunc::Parms + 0)->is_ptr();
  //assert(Type::cmp(_igvn.type(n), obj_type) == 0, "type must be same as input's type");

  Node* ctrl_out;
  Node* barrier;
  if (ShenandoahSupport::can_eliminate_barrier(&_igvn, obj_type, n)) {
    /*
    tty->print("eliminated barrier during expand_macros");
    barrier->dump();
    */
    //assert(false, "does that still happen?");
    // We know it's null. Eliminate the barrier.
    // TODO: This should actually be done in an early phase: eliminate-macros.
    barrier = n;
    ctrl_out = ctrl;
  } else if (obj_type->meet(TypePtr::NULL_PTR) == obj_type) {
    // We don't know if it's null or not. Need to create a null-check around the barrier.
    Node* null = zerocon(T_OBJECT);

    // Make the merge point.
    enum { _obj_path = 1, _null_path, PATH_LIMIT };
    RegionNode* region = new RegionNode(PATH_LIMIT);
    Node*       phi    = new PhiNode(region, obj_type);

    // Construct the null check.
    Node* chk = _igvn.transform(new CmpPNode(n, null));
    Node* test = _igvn.transform(new BoolNode(chk, BoolTest::ne));
    IfNode* iff = _igvn.transform(new IfNode(ctrl, test, PROB_LIKELY_MAG(3), COUNT_UNKNOWN))->as_If();
    //igvn.set_type(iff, iff->Value(&igvn));
    //iff->init_flags(Node::Flag_is_shenandoah_rb);
    Node* iftrue = _igvn.transform(new IfTrueNode(iff));
    Node* iffalse = _igvn.transform(new IfFalseNode(iff));

    // Null path.
    region->init_req(_null_path, iffalse);
    phi   ->init_req(_null_path, null); // Set null path value

    Node* rb = make_basic_read_barrier(n, iftrue, mem, obj_type);

    region->init_req(_obj_path, iftrue);
    phi   ->init_req(_obj_path, rb);
    barrier = _igvn.transform(phi);
    ctrl_out = _igvn.transform(region);
  } else {
    /*
    if (late_type->meet(TypePtr::NULL_PTR) == obj_type) {
      tty->print_cr("late type looses non-null info:");
      late_type->dump();
      tty->print_cr("\nearly type:");
      obj_type->dump();
      tty->print_cr(" ");
    }
    */
    // We know it's not null. Very common case, no null-check needed.
    barrier = make_basic_read_barrier(n, ctrl, mem, obj_type);
    ctrl_out = ctrl;
  }
  /*
  if (! late_type->eq(obj_type)) {
    tty->print_cr("late changed (narrowed?):");
    late_type->dump();
    tty->print_cr("\nearly type:");
    obj_type->dump();
    tty->print_cr(" ");
  }
  */
  for (DUIterator_Last jmin, j = rb->last_outs(jmin); j >= jmin; --j) {
    Node *p = rb->last_out(j);
    assert(p->is_Proj(), "expect projection");
    ProjNode* proj = p->as_Proj();
    if (proj->_con == TypeFunc::Control) {
      _igvn.replace_node(proj, ctrl_out);
    } else if (proj->_con == TypeFunc::Memory) {
      /*
      tty->print_cr("replacing out mem with: ");
      mem->dump();
      */
      _igvn.replace_node(proj, mem);
    } else if (proj->_con == TypeFunc::Parms + 0) {
      _igvn.replace_node(proj, barrier);
    } else {
      ShouldNotReachHere();
    }
  }
}

Node* PhaseMacroExpand::make_basic_write_barrier(CallNode* wb, Node* n, Node* ctrl, Node* mem, const TypePtr* obj_type, Node*& ctrl_out, Node*& mem_out)
{
  // Construct check for evacuation-in-progress.
  Node* jthread = _igvn.transform(new ThreadLocalNode());
  Node* evac_in_progr_addr = _igvn.transform(new AddPNode(top(), jthread, longcon(in_bytes(JavaThread::evacuation_in_progress_offset()))));

  Node* evac_in_progr = _igvn.transform(new LoadUBNode(ctrl, mem, evac_in_progr_addr, TypeRawPtr::BOTTOM, TypeInt::BOOL, MemNode::unordered));

  n = make_basic_read_barrier(n, ctrl, mem, obj_type);

  Node* chk = _igvn.transform(new CmpINode(evac_in_progr, intcon(0)));
  Node* test = _igvn.transform(new BoolNode(chk, BoolTest::eq));

  // Make the merge point.
  enum { _evac_path = 1, _no_evac_path, PATH_LIMIT };
  RegionNode* region = new RegionNode(PATH_LIMIT);
  Node*       phi    = new PhiNode(region, obj_type);
  Node* mem_phi = new PhiNode(region, Type::MEMORY, TypePtr::BOTTOM /*mem->adr_type()*/);

  // Make the actual if-branch.
  IfNode* iff = _igvn.transform(new IfNode(ctrl, test, PROB_LIKELY_MAG(1), COUNT_UNKNOWN))->as_If();
  Node* iftrue = _igvn.transform(new IfTrueNode(iff));
  Node* iffalse = _igvn.transform(new IfFalseNode(iff));

  // No-evacuation path.
  region->init_req(_no_evac_path, iftrue);
  phi->init_req(_no_evac_path, n);
  mem_phi->init_req(_no_evac_path, mem);

  // Evacuation path.
  //const TypePtr* adr_type = TypeRawPtr::BOTTOM;
  const TypePtr* adr_type = obj_type->add_offset(-8);
  //tty->print("wb adr_type: "); adr_type->dump(); tty->print("\n");

  CallNode *call = new CallStaticJavaNode(OptoRuntime::shenandoah_barrier_Type(obj_type),
					  OptoRuntime::shenandoah_write_barrier_Java(),
					  "shenandoah_write_barrier",
					  wb->jvms()->bci(), adr_type);

  // Call inputs.
  copy_predefined_input_for_runtime_call(iffalse, wb, call);
  call->init_req(TypeFunc::Parms + 0, n);
  //call->set_jvms(wb->jvms() != NULL ? wb->jvms()->clone_deep(C) : NULL);
  copy_call_debug_info(wb, call);
  //_igvn.replace_node(wb, call);
  transform_later(call); // = _igvn.transform(call)->as_Call();

  // Call outputs.
  Node* ctl_out = _igvn.transform(new ProjNode(call, TypeFunc::Control));
  Node* barrier_out = _igvn.transform(new ProjNode(call, TypeFunc::Parms + 0));
  Node* mem_call_out = _igvn.transform(new ProjNode(call, TypeFunc::Memory));

  MergeMemNode* merge_mem_out = MergeMemNode::make(mem);
  merge_mem_out->set_memory_at(C->get_alias_index(obj_type->add_offset(-8)), mem_call_out);
  mem_out = _igvn.transform(merge_mem_out);

  region->init_req(_evac_path, ctl_out);
  phi->init_req(_evac_path, barrier_out);
  mem_phi->init_req(_evac_path, mem_out);
  //assert(call->jvms()->method() != NULL, "must have method in JVMS");

  mem_out = _igvn.transform(mem_phi);
  ctrl_out = _igvn.transform(region);
  phi = _igvn.transform(phi);
  return phi;
}

void PhaseMacroExpand::expand_write_barrier(ShenandoahBarrierNode* wb) {

  Node* n = wb->in(TypeFunc::Parms + 0);
  Node* ctrl = wb->in(TypeFunc::Control);
  Node* mem = wb->in(TypeFunc::Memory);

  //const TypePtr* obj_type = _igvn.type(n)->is_ptr();
  const TypePtr* obj_type = _igvn.type(wb)->is_tuple()->field_at(TypeFunc::Parms + 0)->is_ptr();
  //assert(Type::cmp(_igvn.type(n), obj_type) == 0, "type must be same as input's type");
  
  Node* ctrl_out = NULL;
  Node* barrier = NULL;
  Node* mem_out = NULL;
  if (ShenandoahSupport::can_eliminate_barrier(&_igvn, obj_type, n)) {
    /*
    tty->print("eliminated barrier during expand_macros");
    barrier->dump();
    */
    //assert(false, "does that still happen?");
    // We know it's null. Eliminate the barrier.
    // TODO: This should actually be done in an early phase: eliminate-macros.
    barrier = n;
    ctrl_out = ctrl;
    mem_out = mem;
  } else if (obj_type->meet(TypePtr::NULL_PTR) == obj_type) {
    // We don't know if it's null or not. Need to create a null-check around the barrier.
    Node* null = zerocon(T_OBJECT);

    // Make the merge point.
    enum { _obj_path = 1, _null_path, PATH_LIMIT };
    RegionNode* region = new RegionNode(PATH_LIMIT);
    Node*       phi    = new PhiNode(region, obj_type);
    Node* mem_phi = new PhiNode(region, Type::MEMORY, TypePtr::BOTTOM /*mem->adr_type()*/);

    // Construct the null check.
    Node* chk = _igvn.transform(new CmpPNode(n, null));
    Node* test = _igvn.transform(new BoolNode(chk, BoolTest::ne));
    IfNode* iff = _igvn.transform(new IfNode(ctrl, test, PROB_LIKELY_MAG(3), COUNT_UNKNOWN))->as_If();
    //igvn.set_type(iff, iff->Value(&igvn));
    //iff->init_flags(Node::Flag_is_shenandoah_wb);
    Node* iftrue = _igvn.transform(new IfTrueNode(iff));
    Node* iffalse = _igvn.transform(new IfFalseNode(iff));

    // Null path.
    region->init_req(_null_path, iffalse);
    phi   ->init_req(_null_path, null); // Set null path value
    mem_phi->init_req(_null_path, mem);

    Node* ctl_out = NULL;
    Node* result = make_basic_write_barrier(wb, n, iftrue, mem, obj_type, ctl_out, mem_out);
    assert(ctl_out != NULL, "expect non-null ctrl");
    assert(mem_out != NULL, "expect non-null memory");
    region->init_req(_obj_path, ctl_out);
    phi   ->init_req(_obj_path, result);
    mem_phi->init_req(_obj_path, mem_out);

    ctrl_out = _igvn.transform(region);
    barrier = _igvn.transform(phi);
    mem_out = _igvn.transform(mem_phi);

  } else {
    // We know it's not null. Very common case, no null-check needed.
    barrier = make_basic_write_barrier(wb, n, ctrl, mem, obj_type, ctrl_out, mem_out);
    assert(ctrl_out != NULL, "expect non-null ctrl");
    assert(mem_out != NULL, "expect non-null memory");
    /*
    tty->print_cr("write barrier:");
    barrier->dump(6);
    tty->print_cr("ctrl out:");
    ctrl_out->dump(2);
    tty->print_cr("mem out:");
    mem_out->dump(2);
    tty->print_cr("original wb:");
    wb->dump(2);
    */
  }
  for (DUIterator_Last jmin, j = wb->last_outs(jmin); j >= jmin; --j) {
    Node *p = wb->last_out(j);
    assert(p->is_Proj(), "expect projection");
    ProjNode* proj = p->as_Proj();
    if (proj->_con == TypeFunc::Control) {
      _igvn.replace_node(proj, ctrl_out);
    } else if (proj->_con == TypeFunc::Memory) {
      _igvn.replace_node(proj, mem_out);
    } else if (proj->_con == TypeFunc::Parms + 0) {
      _igvn.replace_node(proj, barrier);
    } else {
      ShouldNotReachHere();
    }
  }
}

void PhaseMacroExpand::expand_barrier(Node* b) {
  if (ShenandoahSupport::write_used(b) > 0) {
    assert(b->isa_ShenandoahBarrier(), "expect non-read-barrier");
    expand_write_barrier(b->as_ShenandoahBarrier());
  } else if (ShenandoahSupport::read_used(b) > 0) {
    expand_read_barrier(b);
  } else {
    ShouldNotReachHere();
  }
}

bool PhaseMacroExpand::eliminate_barrier_node(Node* barrier) {

  if (! ShenandoahSupport::can_get_barrier_input(barrier)) {
    return false;
  }

  Node* n = ShenandoahSupport::barrier_input(barrier);
  const Type* obj_type = _igvn.type(n);
  if ((! ShenandoahSupport::is_keep(barrier)) || ShenandoahSupport::can_eliminate_barrier(&_igvn, obj_type, n)) {
    ShenandoahSupport::eliminate_barrier(_igvn, barrier);
    return true;
  }
  return false;
}

class IncreaseReadBarrierVisitor : public ShenandoahBarrierVisitor {
public:
  void push_barrier(Node* b) {
    if (b->Opcode() == Op_ShenandoahReadBarrier) {
      ((ShenandoahReadBarrierNode*) b)->increase_read_used();
    } else if (b->Opcode() == Op_ShenandoahBarrier) {
      ((ShenandoahBarrierNode*) b)->increase_read_used();
    } else {
      ShouldNotReachHere();
    }
  }
};

class IncreaseWriteBarrierVisitor : public ShenandoahBarrierVisitor {
public:
  void push_barrier(Node* b) {
    if (b->Opcode() == Op_ShenandoahReadBarrier) {
      ((ShenandoahReadBarrierNode*) b)->increase_write_used();
    } else if (b->Opcode() == Op_ShenandoahBarrier) {
      ((ShenandoahBarrierNode*) b)->increase_write_used();
    } else {
      ShouldNotReachHere();
    }
  }
};

void ShenandoahSupport::follow_through_phis(Node* phi, ShenandoahBarrierVisitor* v, GrowableArray<Node*>* phistack) {
  assert(phi->isa_Phi(), "expect phi");
  if (! phistack->contains(phi)) {
    phistack->push(phi);
    for (uint i = phi->req() - 1; i >= PhiNode::Input; i--) {
      visit_barrier_chain(phi->in(i), v, phistack);
    }
    phistack->pop();
  }
}


void ShenandoahSupport::visit_barrier_chain(Node* n, ShenandoahBarrierVisitor* v, GrowableArray<Node*>* phistack) {

  //tty->print_cr("barrier chain:");
  //if (n != NULL) n->dump(); else tty->print_cr("NULL");

  if (n == NULL) {
    v->leaf(true); // Dunno.
  } else if (is_shenandoah_barrier(n)) {
    Node* in = barrier_input(n);
    ResourceMark rm;
    v->push_barrier(n);
    visit_barrier_chain(in, v, phistack);
    v->pop_barrier(n);
  } else if (n->isa_Proj()) {
    n = n->in(0);
    visit_barrier_chain(n, v, phistack);
  } else if (n->isa_ConstraintCast()) {
    assert(n->Opcode() == Op_CastPP, "expect CastPP");
    n = n->in(1);
    visit_barrier_chain(n, v, phistack);
  } else if (n->isa_CheckCastPP()) {
    n = n->in(1);
    visit_barrier_chain(n, v, phistack);
  } else if (n->isa_Phi()) {
    assert(phistack != NULL, "need phistack");
    follow_through_phis(n, v, phistack);
  } else if (n->isa_Load()) {
    assert(n->Opcode() == Op_LoadP, "expect LoadP");
    v->leaf(true);
  } else if (n->isa_Start()) {
    v->leaf(true);
  } else if (n->is_Con()) {
#ifdef ASSERT
    const Type* t = n->bottom_type(); // Maybe use igvn type!
    assert(t->higher_equal(TypePtr::NULL_PTR) || t->is_oopptr()->const_oop() != NULL, "expect null or constant oop");
#endif
    v->leaf(false);
  } else if (n->is_Allocate()) {
    v->leaf(false);
  } else if (n->isa_CallStaticJava()) {
    CallStaticJavaNode* csj = n->as_CallStaticJava();
    if (csj->_name != NULL && strncmp(csj->_name, "_multianewarray", 15) == 0) {
      v->leaf(false);
    } else {
      v->leaf(true);
    }
  } else if (n->isa_CallJava()) {
    v->leaf(true);
  } else if (n->Opcode() == Op_CreateEx) {
    v->leaf(false);
  } else {
#ifdef ASSERT
    tty->print_cr("what node is this?");
    n->dump(2);
    Unimplemented();
#endif
  }
}

void ShenandoahSupport::increase_read_used(PhaseIterGVN& igvn, Node* n) {
  Arena* a = igvn.C->comp_arena();
  GrowableArray<Node*>* phistack = new (a) GrowableArray<Node*>(a, 4, 0, NULL);
  IncreaseReadBarrierVisitor v;
  visit_barrier_chain(n, &v, phistack);
}

void ShenandoahSupport::increase_write_used(PhaseIterGVN& igvn, Node* n) {
  Arena* a = igvn.C->comp_arena();
  GrowableArray<Node*>* phistack = new (a) GrowableArray<Node*>(a, 4, 0, NULL);
  IncreaseWriteBarrierVisitor v;
  visit_barrier_chain(n, &v, phistack);
}

class FindHighestUseVisitor : public ShenandoahBarrierVisitor {

public:
  uint _write_used;
  uint _read_used;
  Node* _highest;

  FindHighestUseVisitor() : _write_used(0), _read_used(0), _highest(NULL) {
  }

  void push_barrier(Node* b) {
    _write_used = MAX2(ShenandoahSupport::write_used(b), _write_used);
    _read_used = MAX2(ShenandoahSupport::read_used(b), _read_used);
    _highest = b;
  }
};

class KeepHighestVisitor : public ShenandoahBarrierVisitor {

private:

  GrowableArray<Node*>* _stack;

public:

  KeepHighestVisitor(GrowableArray<Node*>* stack) : _stack(stack) {
  }

  void push_barrier(Node* b) {
    assert(ShenandoahSupport::is_shenandoah_barrier(b), "expect Shenandoah barrier");
    _stack->push(b);
  }

  void pop_barrier(Node* b) {
    Node* top = _stack->pop();
    assert(b == top, "consistency");
  }

  void leaf(bool need_barrier) {
    if (need_barrier) {
      Node* top = _stack->top();
#ifdef ASSERT
      if (_stack->length() == 1) {
	tty->print_cr("where does this come from?");
	top->dump(4);
	ShouldNotReachHere();
      }
#endif
      assert(ShenandoahSupport::is_shenandoah_barrier(top), "expect Shenandoah barrier");
      ShenandoahSupport::set_keep(top, true);
    }
  }

};

void ShenandoahSupport::visit_memory_chain_through_phi(Node* phi, int alias, ShenandoahMemoryVisitor* v, GrowableArray<Node*>* phistack) {
  assert(phi->isa_Phi(), "expect phi");
  if (! phistack->contains(phi)) {
    phistack->push(phi);
    for (uint i = phi->req() - 1; i >= PhiNode::Input; i--) {
      visit_memory_chain(phi->in(i), alias, v, phistack);
    }
    phistack->pop();
  } // else maybe call leaf.
}

void ShenandoahSupport::visit_memory_chain(Node* mem, int alias, ShenandoahMemoryVisitor* v, GrowableArray<Node*>* phistack) {
  if (mem == NULL) { // ??
  } else if (mem->isa_MergeMem()) {
    MergeMemNode* mmem = mem->as_MergeMem();
    Node* up = mmem->memory_at(alias);
    visit_memory_chain(up, alias, v, phistack);
  } else if (mem->isa_Phi()) {
    visit_memory_chain_through_phi(mem, alias, v, phistack);
  } else if (mem->is_Proj()) {
    visit_memory_chain(mem->in(0), alias, v, phistack);
  } else if (mem->isa_ShenandoahBarrier()) {
    visit_memory_chain(mem->in(TypeFunc::Memory), alias, v, phistack);
  } else if (mem->isa_Start()) {
    v->leaf(false);
  } else if (mem->isa_Store()) {
    v->leaf(true);
  } else if (mem->isa_MemBar()) {
    v->leaf(true);
  } else if (mem->isa_ArrayCopy()) {
    v->leaf(true);
  } else if (mem->isa_CallJava()) {
    v->leaf(true);
  } else if (mem->isa_Allocate()) {
    v->leaf(false);
  } else if (mem->isa_CallRuntime()) {
    v->leaf(true);
  } else {
#ifdef ASSERT
    tty->print_cr("what mem is this:");
    mem->dump(2);
    Unimplemented();
#endif
  }
}

class AnalyzeMemoryVisitor : public ShenandoahMemoryVisitor {
private:
  bool _polluted;
public:
  AnalyzeMemoryVisitor() : _polluted(false) {
  }
  void leaf(bool polluted) {
    _polluted = _polluted | polluted;
  }
  bool is_polluted() {
    return _polluted;
  }
};

void ShenandoahSupport::optimize_barrier(PhaseIterGVN& igvn, Node* b) {

  Arena* a = igvn.C->comp_arena();
  GrowableArray<Node*>* phistack = new (a) GrowableArray<Node*>(a, 4, 0, NULL);
  FindHighestUseVisitor v;
  visit_barrier_chain(b, &v, phistack);
  ShenandoahBarrierNode* keep = NULL;
  //assert(v._write_used == 1 || v._read_used == 1, "for now..");

  if (v._write_used >= 1) {
    //tty->print_cr("keep highest writer");
    // More than one write user, keep highest.
    if (!is_keep(b)) {
      // Arena-allocated because nested phi stack would mess it up, when stack-allocated.
      GrowableArray<Node*>* stack = new (a) GrowableArray<Node*>(a, 4, 0, NULL);
      KeepHighestVisitor keep(stack);
      visit_barrier_chain(b, &keep, phistack);
    }
    //b->set_keep(true);
  } else if (v._read_used >= 1) {
    bool keep_lowest = false;
    if (!is_keep(b)) {
      if (slice(b) != NULL) {
	AnalyzeMemoryVisitor v;
	int alias = igvn.C->get_alias_index(slice(b));
	Node* mem;
	if (b->Opcode() == Op_ShenandoahReadBarrier) {
	  mem = b->in(MemNode::Memory);
	} else if (b->Opcode() == Op_ShenandoahBarrier) {
	  mem = b->in(TypeFunc::Memory);
	} else {
	  ShouldNotReachHere();
	  mem = NULL;
	}
	visit_memory_chain(mem, alias, &v, phistack);

	if (v.is_polluted()) {
	  keep_lowest = true;
	} else {
	  //tty->print_cr("provably no store to our slice, yay!");
	}
      } else {
	keep_lowest = false;
      }
    } else { // Volatile reads.
      keep_lowest = true;
    }
    if (keep_lowest) {
      set_keep(b, true);
    } else {
      // Arena-allocated because nested phi stack would mess it up, when stack-allocated.
      GrowableArray<Node*>* stack = new (a) GrowableArray<Node*>(a, 4, 0, NULL);
      KeepHighestVisitor keep(stack);
      visit_barrier_chain(b, &keep, phistack);
    }
  } // Else no user, eliminate all in chain.

}

void ShenandoahSupport::eliminate_barrier(PhaseIterGVN& igvn, Node* b) {

  if (b->Opcode() == Op_ShenandoahBarrier) {
    //tty->print("eliminated: ");  b->dump();
    for (DUIterator_Last jmin, j = b->last_outs(jmin); j >= jmin; --j) {
      Node *p = b->last_out(j);
      assert(p->is_Proj(), "expect projection");
      ProjNode* proj = p->as_Proj();
      if (proj->_con == TypeFunc::Control) {
	igvn.replace_node(proj, b->in(TypeFunc::Control));
      } else if (proj->_con == TypeFunc::Memory) {
	igvn.replace_node(proj, b->in(TypeFunc::Memory));
      } else if (proj->_con == TypeFunc::Parms + 0) {
	igvn.replace_node(proj, b->in(TypeFunc::Parms + 0));
      } else {
	ShouldNotReachHere();
      }
    }
  } else if (b->Opcode() == Op_ShenandoahReadBarrier) {
    if (b->in(MemNode::Address)->is_AddP()) {
      Node* in = barrier_input(b);
      igvn.replace_node(b, in);
    }
  } else {
    ShouldNotReachHere();
  }
}

bool ShenandoahSupport::is_shenandoah_barrier(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return true;
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return true;
  }
  return false;
}

bool ShenandoahSupport::is_use_read(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return ((ShenandoahReadBarrierNode*) n)->is_use_read();
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->is_use_read();
  }
  ShouldNotReachHere();
  return false;
}

bool ShenandoahSupport::is_use_write(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return false;
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->is_use_write();
  }
  ShouldNotReachHere();
  return false;
}

bool ShenandoahSupport::is_keep(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return ((ShenandoahReadBarrierNode*) n)->is_keep();
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->is_keep();
  }
  ShouldNotReachHere();
  return false;
}

void ShenandoahSupport::set_keep(Node* n, bool keep) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    ((ShenandoahReadBarrierNode*) n)->set_keep(keep);
    return;
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    ((ShenandoahBarrierNode*) n)->set_keep(keep);
    return;
  }
  ShouldNotReachHere();
}

const TypePtr* ShenandoahSupport::slice(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return ((ShenandoahReadBarrierNode*) n)->slice();
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->slice();
  }
  ShouldNotReachHere();
  return NULL;
}

uint ShenandoahSupport::read_used(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return ((ShenandoahReadBarrierNode*) n)->read_used();
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->read_used();
  }
  ShouldNotReachHere();
  return -1;
}

uint ShenandoahSupport::write_used(Node* n) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    return ((ShenandoahReadBarrierNode*) n)->write_used();
  } else if (n->Opcode() == Op_ShenandoahBarrier) {
    return ((ShenandoahBarrierNode*) n)->write_used();
  }
  ShouldNotReachHere();
  return -1;
}

bool ShenandoahSupport::can_get_barrier_input(Node* b) {
  return b->Opcode() == Op_ShenandoahBarrier ||
    (b->Opcode() == Op_ShenandoahReadBarrier && b->in(MemNode::Address)->is_AddP());
}

Node* ShenandoahSupport::barrier_input(Node* b) {
  if (b->Opcode() == Op_ShenandoahReadBarrier) {
    Node* addr = b->in(MemNode::Address);
    assert(addr->is_AddP(), "expect AddP");
    assert(addr->in(AddPNode::Base) == addr->in(AddPNode::Address), "base and address must match");
    return addr->in(AddPNode::Base);
  } else if (b->Opcode() == Op_ShenandoahBarrier) {
    return b->in(TypeFunc::Parms + 0);
  }
  ShouldNotReachHere();
  return NULL;
}

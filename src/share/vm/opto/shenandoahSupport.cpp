
#include "opto/addnode.hpp"
#include "opto/callnode.hpp"
#include "opto/memnode.hpp"
#include "opto/node.hpp"
#include "opto/shenandoahSupport.hpp"

Node* ShenandoahSupport::skip_through_read_barrier(Node* n) {
  if (n != NULL && n->Opcode() == Op_LoadP) {
    Node* adr = n->in(LoadNode::Address);
    if (adr != NULL && adr->is_AddP()) {
      Node* offset = adr->in(AddPNode::Offset);
      if (offset != NULL && offset->Opcode() == Op_ConL && offset->get_long() == -8) {
        Node* base = adr->in(AddPNode::Base);
        assert(base != NULL, "cannot be");
        return base;
      }
    }
  }
  return n;
}

Node* ShenandoahSupport::skip_through_write_barrier(Node* n) {
  if (n != NULL && n->Opcode() == Op_Phi) {
    Node* n1 = n->in(1); // _evac_path.
    if (n1 != NULL && n1->Opcode() == Op_Proj) {
      Node* n2 = n1->in(0);
      if (n2 != NULL && n2->Opcode() == Op_CallStaticJava) {
        CallStaticJavaNode* call = n2->as_CallStaticJava();
        if (call != NULL && call->_name != NULL && strcmp(call->_name, "shenandoah_write_barrier") == 0) {
          Node* parm0 = n2->in(TypeFunc::Parms+0);
          assert(parm0 != NULL, "cannot be");
          return skip_through_read_barrier(parm0);
        }
      }
    }
  }
  return n;
}

Node* ShenandoahSupport::skip_through_barrier(Node* n) {
  Node* skipped = skip_through_read_barrier(n);
  if (skipped == n) {
    skipped = skip_through_write_barrier(n);
  }
  // Skip through chains of barriers
  if (skipped != n) {
    skipped = skip_through_barrier(skipped);
  }
  return skipped;
}

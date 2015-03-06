#ifndef SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP
#define SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP

#include "memory/allocation.hpp"

class Node;

class ShenandoahSupport : AllStatic {
private:
  static Node* skip_through_read_barrier(Node* n);
  static Node* skip_through_write_barrier(Node* n);
public:
  static Node* skip_through_barrier(Node* n);
};

#endif // SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP

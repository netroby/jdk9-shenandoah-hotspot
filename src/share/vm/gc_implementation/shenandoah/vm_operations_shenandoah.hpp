/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP

#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"

// VM_operations for the Shenandoah Collector.
// For now we are just doing two pauses.  The initial marking pause, and the final finish up marking and perform evacuation pause.
//    VM_ShenandoahInitMark
//    VM_ShenandoahFinishMark

class VM_ShenandoahInitMark: public VM_Operation {
  
public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;
};

class VM_ShenandoahReferenceOperation : public VM_Operation {
  bool doit_prologue();
  void doit_epilogue();

};

class VM_ShenandoahFinishMark: public VM_ShenandoahReferenceOperation {

 public:
  VMOp_Type type() const;
  void doit();
  const char* name() const;

};

class VM_ShenandoahFullGC : public VM_Operation {
 public:
  VMOp_Type type() const;
  void doit();
  const char* name() const;
};

class VM_ShenandoahVerifyHeapAfterEvacuation: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

class VM_ShenandoahEvacuation: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

class VM_ShenandoahVerifyHeapAfterUpdateRefs: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

class VM_ShenandoahUpdateRootRefs: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

class VM_ShenandoahUpdateRefs: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

#endif //SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP

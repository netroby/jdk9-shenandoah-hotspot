#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP

#include "memory/genOopClosures.hpp"


// Applies the given oop closure to all oops in all klasses visited.
class ShenandoahKlassClosure : public KlassClosure {
  friend class ShenandoahOopClosure;
  OopClosure* _oop_closure;

  // Used when _oop_closure couldn't be set in an initialization list.
  void initialize(OopClosure* oop_closure) {
    assert(_oop_closure == NULL, "Should only be called once");
    _oop_closure = oop_closure;
  }
 public:
  ShenandoahKlassClosure(OopClosure* oop_closure = NULL) : _oop_closure(oop_closure) { }

  void do_klass(Klass* k) {
    k->print();
  }
};

// The base class for all CMS marking closures.
// It's used to proxy through the metadata to the oops defined in them.
class ShenandoahOopClosure: public ExtendedOopClosure {
  ShenandoahKlassClosure      _klass_closure;
 public:
  ShenandoahOopClosure() : ExtendedOopClosure() {
    _klass_closure.initialize(this);
  }
  ShenandoahOopClosure(ReferenceProcessor* rp) : ExtendedOopClosure(rp) {
    _klass_closure.initialize(this);
  }

  virtual bool do_metadata()    { return do_metadata_nv(); }
  inline  bool do_metadata_nv() { return true; }

  virtual void do_klass(Klass* k);
  void do_klass_nv(Klass* k);

  virtual void do_class_loader_data(ClassLoaderData* cld);
};

#endif //  SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP

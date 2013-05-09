#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_QUEUES_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_QUEUES_HPP

#include "oops/oop.hpp"
#include "utilities/globalDefinitions.hpp"

// We aren't using these queues yet, but I'm keeping them to do performance studies.


struct fields {
  uint32_t _top;
  uint32_t _tag;
};

union SCMAge_t {
  uint64_t _data;
  fields _fields;

  uint32_t top() {return _fields._top;}
  uint32_t tag() {return _fields._tag;}
  void set_top(uint32_t top) { _fields._top = top;}
  void set_tag(uint32_t tag) { _fields._tag = tag;}
  void increment_top() {_fields._top++;}
  uint64_t value() {return _data;}
  void set_value(uint64_t value) {_data = value;}
  void set_value(volatile SCMAge_t val) {_data = val._data;}
} SCMAge;

    
class SCMTaskQueue {
private:  
  // This value needs to be CAS'd in one instruction.
  // It contains 32 bits of top and 32 bits of age.
  volatile SCMAge_t age;
  volatile uint32_t bot;
  oop deq[10000];

  void cas(volatile uint64_t* location ,uint64_t old_age, uint64_t new_age) {
    Atomic::cmpxchg((jlong)new_age, (jlong*)location, (jlong)old_age);
  }

  void cas(volatile SCMAge_t* location, SCMAge_t old_age, SCMAge_t new_age) {
    cas((uint64_t*) location, old_age.value(), new_age.value());
  }

 public:
  void pushBottom(oop obj);
  oop popBottom();
  oop popTop();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_QUEUES_HPP

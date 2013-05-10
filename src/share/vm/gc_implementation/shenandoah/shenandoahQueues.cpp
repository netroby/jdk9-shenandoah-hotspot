#include "gc_implementation/shenandoah/shenandoahQueues.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.hpp"

// Included for the atomics.  Can be cleaned up.

#include "utilities/taskqueue.hpp"

void SCMTaskQueue::pushBottom(oop obj) {
     uint32_t localBot = bot;
     deq[localBot++] = obj;
     bot = localBot;
   }

oop SCMTaskQueue::popBottom() {
  SCMAge_t oldAge;
  SCMAge_t newAge;

  uint32_t localBot = bot;
  if (localBot == 0)
    return NULL;
  localBot--;
  bot = localBot;
  OrderAccess::fence();
  oop obj = deq[localBot];
  oldAge._data = age._data;
  if (localBot > oldAge.top())
    return obj;
  bot = 0;
  newAge.set_top(0);
  newAge.set_tag(oldAge.tag() + 1);
  if (localBot == oldAge.top()) {
    cas(&age, oldAge, newAge);
    if (oldAge.value() == newAge.value())
      return obj;
  } 
  age._data = newAge._data;
  return NULL;
}

oop SCMTaskQueue::popTop() {
  SCMAge_t oldAge;
  SCMAge_t newAge;

  oldAge._data = age._data;
  uint32_t localBot = bot;

  if (localBot <= oldAge.top()) 
    return NULL;
  oop obj = deq[oldAge.top()];
  newAge = oldAge;
  newAge.increment_top();
  cas(&age, oldAge, newAge);
  if (oldAge.value() == newAge.value())
    return obj;
  return NULL;
}

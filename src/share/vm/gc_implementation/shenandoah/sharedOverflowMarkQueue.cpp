/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/sharedOverflowMarkQueue.hpp"
#include "runtime/mutexLocker.hpp"

SharedOverflowMarkQueue::SharedOverflowMarkQueue() :
  _first(NULL),
  _mutex(Mutex::leaf, "SharedOverflowMarkQueue lock", true) {

  // Nothing else to do.
 }

void SharedOverflowMarkQueue::push(oop o) {
  MutexLocker ml(&_mutex);
  {
    SOMQItem* item = new SOMQItem();
    item->_obj = o;
    item->_next = _first;
    _first = item;
  }
}

oop SharedOverflowMarkQueue::pop() {
  MutexLocker ml(&_mutex);
  {
    oop o = NULL;
    if (_first != NULL) {
      SOMQItem* item = _first;
      o = item->_obj;
      _first = item->_next;
      delete item;
    }
    return o;
  }
}

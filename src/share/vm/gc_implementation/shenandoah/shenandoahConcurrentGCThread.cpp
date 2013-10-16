
#include "gc_implementation/shenandoah/shenandoahConcurrentGCThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "runtime/vmThread.hpp"

ShenandoahConcurrentGCThread::ShenandoahConcurrentGCThread() :
  ConcurrentGCThread() {
}

ShenandoahConcurrentGCThread::~ShenandoahConcurrentGCThread() {
}

void ShenandoahConcurrentGCThread::run() {
  initialize_in_thread();
  wait_for_universe_init();

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  // These are just arbitrary numbers for now.  CHF
  size_t targetStartMarking = heap->capacity() / 64;
  size_t targetBytesAllocated = ShenandoahHeapRegion::RegionSizeBytes;

  while (true) {
    
    if (heap->used() > targetStartMarking && heap->_bytesAllocSinceCM > targetBytesAllocated) {
      heap->_bytesAllocSinceCM = 0;
      if (ShenandoahGCVerbose) 
        tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT" Target = "SIZE_FORMAT" doing initMark\n", heap->capacity(), heap->used(), targetStartMarking);
 
      if (ShenandoahGCVerbose) tty->print("Starting a mark");

      VM_ShenandoahInitMark initMark;
      VMThread::execute(&initMark);

      ShenandoahHeap::heap()->concurrentMark()->markFromRoots();

      VM_ShenandoahFinishMark finishMark;
      VMThread::execute(&finishMark);

    } else {
      yield();
    }

  }
}

void ShenandoahConcurrentGCThread::yield() {
  _sts.yield("Concurrent Mark");
}

void ShenandoahConcurrentGCThread::start() {
  create_and_start();
}


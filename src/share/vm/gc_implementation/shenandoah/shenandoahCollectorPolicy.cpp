#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahHeuristics : public CHeapObj<mtGC> {
private:
  NumberSeq _init_mark_ms;
  NumberSeq _final_mark_ms;
  NumberSeq _concurrent_marking_times_ms;
  NumberSeq _concurrent_evacuation_times_ms;

  NumberSeq _allocation_rate_bytes;
  NumberSeq _reclamation_rate_bytes;

  double _init_mark_start;
  double _concurrent_mark_start;
  double _final_mark_start;
  double _concurrent_evacuation_start; 

  int _init_mark_count;
  int _final_mark_count;
  int _concurrent_evacuation_count;

  size_t _bytes_allocated_since_CM;
  size_t _bytes_reclaimed_this_cycle;

public:

  ShenandoahHeuristics();

  void record_init_mark_start();
  void record_init_mark_end();
  void record_concurrent_mark_start();
  void record_concurrent_mark_end();
  void record_final_mark_start();
  void record_final_mark_end();
  void record_concurrent_evacuation_start();
  void record_concurrent_evacuation_end();  
  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const=0;
  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set, 
                                               ShenandoahHeapRegionSet* collection_set, 
                                               ShenandoahHeapRegionSet* free_set) const=0;
  void print_tracing_info();
};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _init_mark_start(0),
  _concurrent_mark_start(0),
  _final_mark_start(0),
  _concurrent_evacuation_start(0),
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _init_mark_count(0),
  _final_mark_count(0),
  _concurrent_evacuation_count(0)
{
  if (PrintGCDetails)
    tty->print_cr("initializing heuristics");
}

void ShenandoahHeuristics::record_init_mark_start() { 
  _init_mark_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_init_mark_end()   { 
  double end = os::elapsedTime();

  double elapsed = (os::elapsedTime() - _init_mark_start); 
  _init_mark_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: InitialMark %d took %lf ms", _init_mark_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_final_mark_start() { 
  _final_mark_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_final_mark_end()   { 
  double elapsed = os::elapsedTime() - _final_mark_start;
  _final_mark_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: FinalMark %d took %lf ms", _final_mark_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_concurrent_evacuation_start() { 
  _concurrent_evacuation_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_concurrent_evacuation_end()   { 
  double elapsed = os::elapsedTime() - _concurrent_evacuation_start;
  _concurrent_evacuation_times_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: Concurrent Evacuation %d took %lf ms", 
		  _concurrent_evacuation_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_bytes_allocated(size_t bytes) {
  _bytes_allocated_since_CM = bytes;
  _allocation_rate_bytes.add(bytes);
}

void ShenandoahHeuristics::record_bytes_reclaimed(size_t bytes) {
  _bytes_reclaimed_this_cycle = bytes;
  _reclamation_rate_bytes.add(bytes);
}

class AggressiveHeuristics : public ShenandoahHeuristics {
public:
  AggressiveHeuristics() : ShenandoahHeuristics(){
  if (PrintGCDetails)
    tty->print_cr("Initializing aggressive heuristics");
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    return true;
  }
  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) const {
    region_set->set_garbage_threshold(8);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

class HalfwayHeuristics : public ShenandoahHeuristics {
public:
  HalfwayHeuristics() : ShenandoahHeuristics() {
  if (PrintGCDetails)
    tty->print_cr("Initializing halfway heuristics");
  }

  bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t threshold_bytes_allocated = heap->capacity() / 4;
    if (used * 2 > capacity && heap->_bytesAllocSinceCM > threshold_bytes_allocated)
      return true;
    else
      return false;
  }
  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                       ShenandoahHeapRegionSet* collection_set,
                                       ShenandoahHeapRegionSet* free_set) const {
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

// GC as little as possible 
class LazyHeuristics : public ShenandoahHeuristics {
public:
  LazyHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing lazy heuristics");
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    size_t targetStartMarking = (capacity / 5) * 4;
    if (used > targetStartMarking) {
      return true;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) const {
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

// These are the heuristics in place when we made this class
class StatusQuoHeuristics : public ShenandoahHeuristics {
public:
  StatusQuoHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing status quo heuristics");
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    size_t targetStartMarking = capacity / 16;
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t threshold_bytes_allocated = heap->capacity() / 4;
    if (used > targetStartMarking
        && heap->_bytesAllocSinceCM > threshold_bytes_allocated) {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      return true;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) const {
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};


ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() {
  initialize_all();
  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: aggressive");
      }
      _heuristics = new AggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "statusquo") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: statusquo");
      }
      _heuristics = new StatusQuoHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "halfway") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: halfway");
      }
      _heuristics = new HalfwayHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "lazy") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: lazy");
      }
      _heuristics = new LazyHeuristics();
    } else {
      fatal("Unknown -XX:ShenandoahGCHeuristics option");
    }
  } else {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: statusquo (default)");
      }
    _heuristics = new StatusQuoHeuristics();
  }
}

ShenandoahCollectorPolicy* ShenandoahCollectorPolicy::as_pgc_policy() {
  return this;
}

ShenandoahCollectorPolicy::Name ShenandoahCollectorPolicy::kind() {
  return CollectorPolicy::ShenandoahCollectorPolicyKind;
}

BarrierSet::Name ShenandoahCollectorPolicy::barrier_set_name() {
  return BarrierSet::ShenandoahBarrierSet;
}

GenRemSet::Name ShenandoahCollectorPolicy::rem_set_name() {
  return GenRemSet::Other;
}

HeapWord* ShenandoahCollectorPolicy::mem_allocate_work(size_t size,
                                                       bool is_tlab,
                                                       bool* gc_overhead_limit_was_exceeded) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

HeapWord* ShenandoahCollectorPolicy::satisfy_failed_allocation(size_t size, bool is_tlab) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

void ShenandoahCollectorPolicy::initialize_alignments() {
  _space_alignment = Arguments::conservative_max_heap_alignment();
  _heap_alignment = Arguments::conservative_max_heap_alignment();
}

void ShenandoahCollectorPolicy::post_heap_initialize() {
  // Nothing to do here (yet).
}

void ShenandoahCollectorPolicy::record_init_mark_start()  { 
  _heuristics->record_init_mark_start();
}

void ShenandoahCollectorPolicy::record_init_mark_end() { 
  _heuristics->record_init_mark_end();
}
void ShenandoahCollectorPolicy::record_final_mark_start() { 
  _heuristics->record_final_mark_start();
}

void ShenandoahCollectorPolicy::record_final_mark_end() { 
  _heuristics->record_final_mark_end();
}

void ShenandoahCollectorPolicy::record_concurrent_evacuation_start() { 
    _heuristics->record_concurrent_evacuation_start();
}

void ShenandoahCollectorPolicy::record_concurrent_evacuation_end()   { 
    _heuristics->record_concurrent_evacuation_end();
}

void ShenandoahCollectorPolicy::record_bytes_allocated(size_t bytes) {
  _heuristics->record_bytes_allocated(bytes);
}
void ShenandoahCollectorPolicy::record_bytes_reclaimed(size_t bytes) {
  _heuristics->record_bytes_reclaimed(bytes);
}

bool ShenandoahCollectorPolicy::should_start_concurrent_mark(size_t used,
							     size_t capacity) {
  _heuristics->should_start_concurrent_mark(used, capacity);
}
  
void ShenandoahCollectorPolicy::choose_collection_and_free_sets(
			     ShenandoahHeapRegionSet* region_set, 
			     ShenandoahHeapRegionSet* collection_set,
                             ShenandoahHeapRegionSet* free_set) {
  _heuristics->choose_collection_and_free_sets(region_set, collection_set, free_set);
}

void ShenandoahCollectorPolicy::print_tracing_info() {
  _heuristics->print_tracing_info();
}

void print_summary(const char* str,
		   const NumberSeq* seq)  {
  double sum = seq->sum();
  gclog_or_tty->print_cr("%-27s = %8.2lf s (avg = %8.2lf ms)",
                str, sum / 1000.0, seq->avg());
}

void print_summary_sd(const char* str,
		      const NumberSeq* seq) {
  print_summary(str, seq);
  gclog_or_tty->print_cr("%+45s = %5d, std dev = %8.2lf ms, max = %8.2lf ms)",
                "(num", seq->num(), seq->sd(), seq->maximum());
}

void ShenandoahHeuristics::print_tracing_info() {
  print_summary_sd("Initial Mark Pauses", &_init_mark_ms);
  print_summary_sd("Final Mark Pauses", &_final_mark_ms);
  print_summary_sd("Concurrent Evacuation Times", 
		   &_concurrent_evacuation_times_ms);
}

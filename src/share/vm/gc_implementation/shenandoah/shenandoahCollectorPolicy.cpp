#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahHeuristics : public CHeapObj<mtGC> {
private:
  NumberSeq _init_mark_ms;
  NumberSeq _final_mark_ms;
  NumberSeq _final_evac_ms;
  NumberSeq _final_uprefs_ms;

  NumberSeq _fullgc_ms;

  NumberSeq _concurrent_mark_times_ms;
  NumberSeq _concurrent_evacuation_times_ms;

  NumberSeq _allocation_rate_bytes;
  NumberSeq _reclamation_rate_bytes;

  double _init_mark_start;
  double _concurrent_mark_start;
  double _final_mark_start;
  double _final_evac_start;
  double _final_uprefs_start;
  double _concurrent_evacuation_start; 
  double _fullgc_start; 

  int _init_mark_count;
  int _final_mark_count;
  int _final_evac_count;
  int _final_uprefs_count;
  int _concurrent_evacuation_count;
  int _concurrent_mark_count;
  int _fullgc_count;

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
  void record_final_evacuation_start();
  void record_final_evacuation_end();
  void record_final_update_refs_start();
  void record_final_update_refs_end();
  void record_concurrent_evacuation_start();
  void record_concurrent_evacuation_end();  
  void record_fullgc_start();
  void record_fullgc_end();
  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const=0;
  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set, 
                                               ShenandoahHeapRegionSet* collection_set, 
                                               ShenandoahHeapRegionSet* free_set) =0;
  void print_tracing_info();
};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _init_mark_start(0),
  _concurrent_mark_start(0),
  _final_mark_start(0),
  _final_evac_start(0),
  _final_uprefs_start(0),
  _concurrent_evacuation_start(0),
  _fullgc_start(0),
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _init_mark_count(0),
  _final_mark_count(0),
  _final_evac_count(0),
  _final_uprefs_count(0),
  _fullgc_count(0),
  _concurrent_mark_count(0),
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
    tty->print_cr("PolicyPrint: InitialMark "INT32_FORMAT" took %lf ms", _init_mark_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_final_mark_start() { 
  _final_mark_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_final_mark_end()   { 
  double elapsed = os::elapsedTime() - _final_mark_start;
  _final_mark_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: FinalMark "INT32_FORMAT" took %lf ms", _final_mark_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_final_evacuation_start() {
  _final_evac_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_final_evacuation_end() {
  double elapsed = os::elapsedTime() - _final_evac_start;
  _final_evac_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails) {
    tty->print_cr("PolicyPrint: FinalEvacuation "INT32_FORMAT" took %lf ms", _final_evac_count++, elapsed * 1000);
  }
}

void ShenandoahHeuristics::record_final_update_refs_start() { 
  _final_uprefs_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_final_update_refs_end()   { 
  double elapsed = os::elapsedTime() - _final_uprefs_start;
  _final_uprefs_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: FinalUpdateRefs "INT32_FORMAT" took %lf ms", _final_uprefs_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_concurrent_evacuation_start() { 
  _concurrent_evacuation_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_concurrent_evacuation_end()   { 
  double elapsed = os::elapsedTime() - _concurrent_evacuation_start;
  _concurrent_evacuation_times_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: Concurrent Evacuation "INT32_FORMAT" took %lf ms", 
		  _concurrent_evacuation_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_concurrent_mark_start() { 
  _concurrent_mark_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_concurrent_mark_end()   { 
  double elapsed = os::elapsedTime() - _concurrent_mark_start;
  _concurrent_mark_times_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: Concurrent Marking "INT32_FORMAT" took %lf ms", 
		  _concurrent_mark_count++, elapsed * 1000);
}

void ShenandoahHeuristics::record_fullgc_start() { 
  _fullgc_start = os::elapsedTime();
}

void ShenandoahHeuristics::record_fullgc_end()   { 
  double elapsed = os::elapsedTime() - _fullgc_start;
  _fullgc_ms.add(elapsed * 1000);
  if (ShenandoahGCVerbose && PrintGCDetails)
    tty->print_cr("PolicyPrint: Full GC "INT32_FORMAT" took %lf ms", 
		  _fullgc_count++, elapsed * 1000);
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
                                               ShenandoahHeapRegionSet* free_set) {
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
                                       ShenandoahHeapRegionSet* free_set) {
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
    } else {
      return false;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) {
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
    } else {
      return false;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) {
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

static uintx clamp(uintx value, uintx min, uintx max) {
  value = MAX2(value, min);
  value = MIN2(value, max);
  return value;
}

static double get_percent(uintx value) {
  double _percent = static_cast<double>(clamp(value, 0, 100));
  return _percent / 100.;
}

class DynamicHeuristics : public ShenandoahHeuristics {
private:
  double _used_threshold_factor;
  double _garbage_threshold_factor;
  double _allocation_threshold_factor;

  uintx _used_threshold;
  uintx _garbage_threshold;
  uintx _allocation_threshold;

public:
  DynamicHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing dynamic heuristics");
    }

    _used_threshold = 0;
    _garbage_threshold = 0;
    _allocation_threshold = 0;

    _used_threshold_factor = 0.;
    _garbage_threshold_factor = 0.;
    _allocation_threshold_factor = 0.;
  }

  virtual ~DynamicHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t targetStartMarking = capacity * _used_threshold_factor;

    size_t threshold_bytes_allocated = heap->capacity() * _allocation_threshold_factor;
    if (used > targetStartMarking &&
        heap->_bytesAllocSinceCM > threshold_bytes_allocated)
    {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    }

    return shouldStartConcurrentMark;
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set)
  {
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes * _garbage_threshold_factor);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }

  void set_used_threshold(uintx used_threshold) {
    this->_used_threshold_factor = get_percent(used_threshold);
    this->_used_threshold = used_threshold;
  }

  void set_garbage_threshold(uintx garbage_threshold) {
    this->_garbage_threshold_factor = get_percent(garbage_threshold);
    this->_garbage_threshold = _garbage_threshold;
  }

  void set_allocation_threshold(uintx allocationThreshold) {
    this->_allocation_threshold_factor = get_percent(allocationThreshold);
    this->_allocation_threshold = allocationThreshold;
  }

  uintx get_allocation_threshold() {
    return this->_allocation_threshold;
  }

  uintx get_garbage_threshold() {
    return this->_garbage_threshold;
  }

  uintx get_used_threshold() {
    return this->_used_threshold;
  }
};


class AdaptiveHeuristics : public ShenandoahHeuristics {
private:
  size_t _max_live_data;
  double _used_threshold_factor;
  double _garbage_threshold_factor;
  double _allocation_threshold_factor;

  uintx _used_threshold;
  uintx _garbage_threshold;
  uintx _allocation_threshold;

public:
  AdaptiveHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing dynamic heuristics");
    }

    _max_live_data = 0;

    _used_threshold = 0;
    _garbage_threshold = 0;
    _allocation_threshold = 0;

    _used_threshold_factor = 0.;
    _garbage_threshold_factor = 0.1;
    _allocation_threshold_factor = 0.;
  }

  virtual ~AdaptiveHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    ShenandoahHeap* _heap = ShenandoahHeap::heap();
    bool shouldStartConcurrentMark = false;

    size_t max_live_data = _max_live_data;
    if (max_live_data == 0) {
      max_live_data = capacity * 0.2; // Very generous initial value.
    } else {
      max_live_data *= 1.3; // Add some wiggle room.
    }
    size_t max_cycle_allocated = _heap->_max_allocated_gc;
    if (max_cycle_allocated == 0) {
      max_cycle_allocated = capacity * 0.3; // Very generous.
    } else {
      max_cycle_allocated *= 1.3; // Add 20% wiggle room. Should be enough.
    }
    size_t threshold = _heap->capacity() - max_cycle_allocated - max_live_data;
    if (used > threshold)
    {
      shouldStartConcurrentMark = true;
    }

    return shouldStartConcurrentMark;
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set)
  {
    size_t bytes_alloc = ShenandoahHeap::heap()->_bytesAllocSinceCM;
    size_t min_garbage =  bytes_alloc/* * 1.1*/;
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes * _garbage_threshold_factor);
    region_set->choose_collection_and_free_sets_min_garbage(collection_set, free_set, min_garbage);
    /*
    tty->print_cr("garbage to be collected: "SIZE_FORMAT, collection_set->garbage());
    tty->print_cr("objects to be evacuated: "SIZE_FORMAT, collection_set->live_data());
    */
    _max_live_data = MAX2(_max_live_data, collection_set->live_data());
  }

  void set_used_threshold(uintx used_threshold) {
    this->_used_threshold_factor = get_percent(used_threshold);
    this->_used_threshold = used_threshold;
  }

  void set_garbage_threshold(uintx garbage_threshold) {
    this->_garbage_threshold_factor = get_percent(garbage_threshold);
    this->_garbage_threshold = _garbage_threshold;
  }

  void set_allocation_threshold(uintx allocationThreshold) {
    this->_allocation_threshold_factor = get_percent(allocationThreshold);
    this->_allocation_threshold = allocationThreshold;
  }

  uintx get_allocation_threshold() {
    return this->_allocation_threshold;
  }

  uintx get_garbage_threshold() {
    return this->_garbage_threshold;
  }

  uintx get_used_threshold() {
    return this->_used_threshold;
  }
};


static DynamicHeuristics *configureDynamicHeuristics() {
  DynamicHeuristics *heuristics = new DynamicHeuristics();

  heuristics->set_garbage_threshold(ShenandoahGarbageThreshold);
  heuristics->set_allocation_threshold(ShenandoahAllocationThreshold);
  heuristics->set_used_threshold(ShenandoahUsedThreshold);
  if (ShenandoahLogConfig) {
    tty->print_cr("Shenandoah dynamic heuristics thresholds: allocation "SIZE_FORMAT", used "SIZE_FORMAT", garbage "SIZE_FORMAT,
                  heuristics->get_allocation_threshold(),
                  heuristics->get_used_threshold(),
                  heuristics->get_garbage_threshold());
  }
  return heuristics;
}

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
    } else if (strcmp(ShenandoahGCHeuristics, "dynamic") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: dynamic");
      }
      _heuristics = configureDynamicHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: adaptive");
      }
      _heuristics = new AdaptiveHeuristics();
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
  
  // This is expected by our algorithm for ShenandoahHeap::heap_region_containing().
  _space_alignment = ShenandoahHeapRegion::RegionSizeBytes;
  _heap_alignment = ShenandoahHeapRegion::RegionSizeBytes;
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

void ShenandoahCollectorPolicy::record_final_evacuation_start() { 
  _heuristics->record_final_evacuation_start();
}

void ShenandoahCollectorPolicy::record_final_evacuation_end() { 
  _heuristics->record_final_evacuation_end();
}

void ShenandoahCollectorPolicy::record_final_update_refs_start() { 
  _heuristics->record_final_update_refs_start();
}

void ShenandoahCollectorPolicy::record_final_update_refs_end() { 
  _heuristics->record_final_update_refs_end();
}

void ShenandoahCollectorPolicy::record_concurrent_evacuation_start() { 
    _heuristics->record_concurrent_evacuation_start();
}

void ShenandoahCollectorPolicy::record_concurrent_evacuation_end()   { 
    _heuristics->record_concurrent_evacuation_end();
}

void ShenandoahCollectorPolicy::record_concurrent_mark_start() { 
    _heuristics->record_concurrent_mark_start();
}

void ShenandoahCollectorPolicy::record_concurrent_mark_end()   { 
    _heuristics->record_concurrent_mark_end();
}

void ShenandoahCollectorPolicy::record_fullgc_start() { 
    _heuristics->record_fullgc_start();
}

void ShenandoahCollectorPolicy::record_fullgc_end()   { 
    _heuristics->record_fullgc_end();
}

void ShenandoahCollectorPolicy::record_bytes_allocated(size_t bytes) {
  _heuristics->record_bytes_allocated(bytes);
}
void ShenandoahCollectorPolicy::record_bytes_reclaimed(size_t bytes) {
  _heuristics->record_bytes_reclaimed(bytes);
}

bool ShenandoahCollectorPolicy::should_start_concurrent_mark(size_t used,
							     size_t capacity) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  return _heuristics->should_start_concurrent_mark(used, capacity);
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
  gclog_or_tty->print_cr("%s = "INT32_FORMAT_W(5)", std dev = %8.2lf ms, max = %8.2lf ms)",
                "(num", seq->num(), seq->sd(), seq->maximum());
}

void ShenandoahHeuristics::print_tracing_info() {
  print_summary_sd("Initial Mark Pauses", &_init_mark_ms);
  print_summary_sd("Final Mark Pauses", &_final_mark_ms);
  print_summary_sd("Final Evacuation Pauses", &_final_evac_ms);
  print_summary_sd("Final Update Refs Pauses", &_final_uprefs_ms);
  print_summary_sd("Concurrent Marking Times", 
		   &_concurrent_mark_times_ms);
  print_summary_sd("Concurrent Evacuation Times", 
		   &_concurrent_evacuation_times_ms);
  print_summary_sd("Full GC Times", 
		   &_fullgc_ms);
}

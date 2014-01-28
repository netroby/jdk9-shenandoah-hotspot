/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "services/shenandoahMemoryPool.hpp"

ShenandoahMemoryPool::ShenandoahMemoryPool(ShenandoahHeap* gen,
					   const char* name,
					   PoolType type,
					   bool support_usage_threshold) :
  CollectedMemoryPool(name, type, gen->capacity_in_bytes(),
                      gen->capacity(),
		      support_usage_threshold),
		      _gen(gen) {
}

MemoryUsage ShenandoahMemoryPool::get_memory_usage() {
  size_t maxSize   = (available_for_allocation() ? max_size() : 0);
  size_t used      = used_in_bytes();
  size_t committed = _gen->capacity_in_bytes();

  return MemoryUsage(initial_size(), used, committed, maxSize);
}

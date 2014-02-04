/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */


#ifndef SHARE_VM_SERVICES_SHENANDOAHMEMORYPOOL_HPP
#define SHARE_VM_SERVICES_SHENANDOAHMEMORYPOOL_HPP

#ifndef SERIALGC
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shared/mutableSpace.hpp"
#include "memory/defNewGeneration.hpp"
#include "memory/heap.hpp"
#include "memory/space.hpp"
#include "services/memoryPool.hpp"
#include "services/memoryUsage.hpp"
#endif

class ShenandoahMemoryPool : public CollectedMemoryPool {
private:

   ShenandoahHeap* _gen;

public:
   
  ShenandoahMemoryPool(ShenandoahHeap* pool,
			  const char* name,
			  PoolType type,
			  bool support_usage_threshold);
  MemoryUsage get_memory_usage();
  size_t used_in_bytes()              { return _gen->used(); }
  size_t max_size() const             { return _gen->max_capacity(); }
};


#endif //SHARE_VM_SERVICES_SHENANDOAHMEMORYPOOL_HPP

#include "precompiled.hpp"
#include "gc/simplegc/simplegcheap.hpp"
#include "gc/simplegc/simpleGcMemoryPool.hpp"

SimpleGCMemoryPool::SimpleGCMemoryPool(SimpleGCHeap* heap) :
        CollectedMemoryPool("SimpleGC Heap",
                            heap->capacity(),
                            heap->max_capacity(),
                            false),
        _heap(heap) {
}

MemoryUsage SimpleGCMemoryPool::get_memory_usage() {
  size_t initial_sz = initial_size();
  size_t max_sz     = max_size();
  size_t used       = used_in_bytes();
  size_t committed  = committed_in_bytes();

  return MemoryUsage(initial_sz, used, committed, max_sz);
}

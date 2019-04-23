/*
 * simpleGcMemoryPool.hpp
 *
 *  Created on: 23-Apr-2019
 *      Author: unmesh
 */

#ifndef SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMEMORYPOOL_HPP_
#define SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMEMORYPOOL_HPP_

#include "gc/simplegc/simplegcheap.hpp"
#include "services/memoryPool.hpp"
#include "services/memoryUsage.hpp"
#include "utilities/macros.hpp"

class SimpleGCMemoryPool : public CollectedMemoryPool {
private:
  SimpleGCHeap* _heap;

public:
  SimpleGCMemoryPool(SimpleGCHeap* heap);
  size_t committed_in_bytes() { return _heap->capacity();     }
  size_t used_in_bytes()      { return _heap->used();         }
  size_t max_size()     const { return _heap->max_capacity(); }
  MemoryUsage get_memory_usage();
};


#endif /* SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMEMORYPOOL_HPP_ */

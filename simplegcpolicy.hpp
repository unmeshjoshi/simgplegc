/*
 * simplegcpolicy.hpp
 *
 *  Created on: 23-Apr-2019
 *      Author: unmesh
 */

#ifndef SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCPOLICY_HPP_
#define SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCPOLICY_HPP_

#include "gc/shared/collectorPolicy.hpp"

class SimpleGCPolicy: public CollectorPolicy {
protected:
  virtual void initialize_alignments() {
    size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
    size_t align = MAX2((size_t)os::vm_allocation_granularity(), page_size);
    _space_alignment = align;
    _heap_alignment  = align;
  }

public:
  SimpleGCPolicy() : CollectorPolicy() {};
};

#endif /* SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCPOLICY_HPP_ */

/*
 * simpleGcMonitoringSupport.hpp
 *
 *  Created on: 23-Apr-2019
 *      Author: unmesh
 */

#ifndef SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMONITORINGSUPPORT_HPP_
#define SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMONITORINGSUPPORT_HPP_


#include "memory/allocation.hpp"

class GenerationCounters;
class SimpleGCHeap;

class SimpleGCMonitoringSupport : public CHeapObj<mtGC> {
private:
  GenerationCounters*   _heap_counters;

public:
  SimpleGCMonitoringSupport (SimpleGCHeap* heap) {
	  //noop
  }
  void update_counters() {
	  //noop
  }
};


#endif /* SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCMONITORINGSUPPORT_HPP_ */

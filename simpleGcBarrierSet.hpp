/*
 * simpleGcBarrierSet.hpp
 *
 *  Created on: 23-Apr-2019
 *      Author: unmesh
 */

#ifndef SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCBARRIERSET_HPP_
#define SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCBARRIERSET_HPP_



#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/barrierSet.hpp"

// No interaction with application is required for SimpleGC, and therefore
// the barrier set is empty.
class SimpleGCBarrierSet: public BarrierSet {
  friend class VMStructs;

public:
  SimpleGCBarrierSet();

  virtual void print_on(outputStream *st) const {}

  virtual void on_thread_create(Thread* thread);
  virtual void on_thread_destroy(Thread* thread);

  template <DecoratorSet decorators, typename BarrierSetT = SimpleGCBarrierSet>
  class AccessBarrier: public BarrierSet::AccessBarrier<decorators, BarrierSetT> {};
};

template<>
struct BarrierSet::GetName<SimpleGCBarrierSet> {
  static const BarrierSet::Name value = BarrierSet::SimpleGCBarrierSet;
};

template<>
struct BarrierSet::GetType<BarrierSet::SimpleGCBarrierSet> {
  typedef ::SimpleGCBarrierSet type;
};



#endif /* SRC_HOTSPOT_SHARE_GC_SIMPLEGC_SIMPLEGCBARRIERSET_HPP_ */

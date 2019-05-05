#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc/simplegc/simplegcheap.hpp"
#include "gc/simplegc/simpleGcMemoryPool.hpp"
#include "gc/simplegc/simpleGcMonitoringSupport.hpp"
#include "gc/simplegc/simpleGcBarrierSet.hpp"
#include "gc/shared/barrierSet.inline.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/markOop.inline.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/thread.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/stack.inline.hpp"
#include "services/management.hpp"
#include "gc/shared/gcArguments.hpp"
#include "logging/log.hpp"

Pair<char*, size_t> SimpleGCHeap::allocate_heap_memory(size_t heap_size_in_bytes, size_t align) {
	// Initialize backing storage
	ReservedSpace heap_rs = Universe::reserve_heap(heap_size_in_bytes, align);
	_virtual_space.initialize(heap_rs, heap_size_in_bytes);

	MemRegion committed_region((HeapWord*) (_virtual_space.low()), (HeapWord*) (_virtual_space.high()));
	initialize_reserved_region(committed_region.start(), committed_region.end());

	_space = new ContiguousSpace();
	_space->initialize(committed_region, /* clear_space = */	true, /* mangle_space = */	true);

	return Pair<char*, size_t>(heap_rs.base(), heap_rs.size());
}

void SimpleGCHeap::init_monitoring_support() {
	// Enable monitoring
	//F
	_monitoring_support = new SimpleGCMonitoringSupport(this);
	_last_counter_update = 0;
	_last_heap_print = 0;
}

void SimpleGCHeap::allocate_marking_bitmap(Pair<char*, size_t> heap_base_address_and_size) {
	size_t bitmap_page_size =
			UseLargePages ?
					(size_t) (os::large_page_size()) :
					(size_t) (os::vm_page_size());
	size_t _bitmap_size = MarkBitMap::compute_size(
			heap_base_address_and_size.second);
	_bitmap_size = align_up(_bitmap_size, bitmap_page_size);
	// Initialize marking bitmap, but not commit it yet
	ReservedSpace bitmap(_bitmap_size, bitmap_page_size);
	MemTracker::record_virtual_memory_type(bitmap.base(), mtGC);
	_bitmap_region = MemRegion((HeapWord*) (bitmap.base()),
			bitmap.size() / HeapWordSize);
	MemRegion heap_region = MemRegion(
			(HeapWord*) (heap_base_address_and_size.first),
			heap_base_address_and_size.second / HeapWordSize);
	_bitmap.initialize(heap_region, _bitmap_region);
}

void SimpleGCHeap::registerBarrier() {
	// Install barrier set
	BarrierSet::set_barrier_set(new SimpleGCBarrierSet());
}

void SimpleGCHeap::log_initial_heap_info() {

}

void SimpleGCHeap::init_heap_log_steps(size_t heap_max_byte_size) {
	size_t UpdateCounterStep = 1 * M;
	_step_counter_update = MIN2<size_t>(heap_max_byte_size / 16,
			UpdateCounterStep);
	size_t PrintHeapSteps = 20;
	_step_heap_print = heap_max_byte_size / PrintHeapSteps;
	size_t TLABDecayTime = 1000;
	_decay_time_ns = (int64_t) (TLABDecayTime) * NANOSECS_PER_MILLISEC;
}

void SimpleGCHeap::init_tlab_size() {
// Precompute hot fields
	_max_tlab_size = MIN2(CollectedHeap::max_tlab_size(),
			align_object_size(4 * M / HeapWordSize));
}

jint SimpleGCHeap::initialize() {
	size_t align = HeapAlignment;
    size_t init_byte_size = align_up(InitialHeapSize, align);
	size_t max_byte_size  = align_up(MaxHeapSize, align);

	// Initialize backing storage
	Pair<char*, size_t> heap_base_address_and_size = allocate_heap_memory(max_byte_size, align);

	init_tlab_size();
	init_heap_log_steps(max_byte_size);
	init_monitoring_support();
	registerBarrier();
	allocate_marking_bitmap(heap_base_address_and_size);

	// All done, print out the configuration
	log_info(gc)("Non-resizeable heap; start/max: " SIZE_FORMAT "M", max_byte_size / M);
	log_info(gc)("Using TLAB allocation; max: " SIZE_FORMAT "K", _max_tlab_size * HeapWordSize / K);

	return JNI_OK;
}

void SimpleGCHeap::post_initialize() {
	CollectedHeap::post_initialize();
}

void SimpleGCHeap::initialize_serviceability() {
	_pool = new SimpleGCMemoryPool(this);
	_memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> SimpleGCHeap::memory_managers() {
	GrowableArray<GCMemoryManager*> memory_managers(1);
	memory_managers.append(&_memory_manager);
	return memory_managers;
}

GrowableArray<MemoryPool*> SimpleGCHeap::memory_pools() {
	GrowableArray<MemoryPool*> memory_pools(1);
	memory_pools.append(_pool);
	return memory_pools;
}

size_t SimpleGCHeap::unsafe_max_tlab_alloc(Thread* thr) const {
	// Return max allocatable TLAB size, and let allocation path figure out
	// the actual TLAB allocation size.
	return _max_tlab_size;
}

SimpleGCHeap* SimpleGCHeap::heap() {
	CollectedHeap* heap = Universe::heap();
	assert(heap != NULL, "Uninitialized access to SimpleGCHeap::heap()");assert(heap->kind() == CollectedHeap::SimpleGC, "Not an SimpleGC heap");
	return (SimpleGCHeap*) heap;
}

void SimpleGCHeap::update_counters() {
	size_t used = _space->used();
	// Allocation successful, update counters

	size_t last = _last_counter_update;
	if ((used - last >= _step_counter_update)
			&& Atomic::cmpxchg(used, &_last_counter_update, last) == last) {
		_monitoring_support->update_counters();
	}

}

void SimpleGCHeap::print_occupancy() {
	size_t used = _space->used();
	// ...and print the occupancy line, if needed
	size_t last = _last_heap_print;
	if ((used - last >= _step_heap_print)
			&& Atomic::cmpxchg(used, &_last_heap_print, last) == last) {
		print_heap_info(used);
		print_metaspace_info();
	}

}

HeapWord* SimpleGCHeap::allocate_work(size_t size) {
	assert(is_object_aligned(size), "Allocation size should be aligned: " SIZE_FORMAT, size);

	HeapWord* res = _space->par_allocate(size);
	size_t space_left = max_capacity() - capacity();

	if ((res == NULL) && (size > space_left)) {
		log_info(gc)("Failed to allocate %d %s bytes",
							(int) byte_size_in_proper_unit(size),
							proper_unit_for_byte_size(size));
		return NULL; //not enough space left. This heap does not support expansion. You need to give ms and mx as same while starting the jvm
	}

	// Allocation successful, update counters
	update_counters();
	// ...and print the occupancy line, if needed
	print_occupancy();

	assert(is_object_aligned(res), "Object should be aligned: " PTR_FORMAT, p2i(res));
	return res;
}

HeapWord* SimpleGCHeap::allocate_new_tlab(size_t min_size,
		size_t requested_size, size_t* actual_size) {
	Thread* thread = Thread::current();
	// Defaults in case elastic paths are not taken
	bool fits = true;
	size_t size = requested_size;
	size_t ergo_tlab = requested_size;
	int64_t time = 0;
	// Always honor boundaries
	size = MAX2(min_size, MIN2(_max_tlab_size, size));

	// Always honor alignment
	size = align_up(size, MinObjAlignment);

	// Check that adjustments did not break local and global invariants
	assert(is_object_aligned(size),
			"Size honors object alignment: " SIZE_FORMAT, size);assert(min_size <= size,
			"Size honors min size: " SIZE_FORMAT " <= " SIZE_FORMAT, min_size, size);assert(size <= _max_tlab_size,
			"Size honors max size: " SIZE_FORMAT " <= " SIZE_FORMAT, size, _max_tlab_size);assert(size <= CollectedHeap::max_tlab_size(),
			"Size honors global max size: " SIZE_FORMAT " <= " SIZE_FORMAT, size, CollectedHeap::max_tlab_size());

	if (log_is_enabled(Trace, gc)) {
		ResourceMark rm;
		log_trace(gc)("TLAB size for \"%s\" (Requested: " SIZE_FORMAT "K, Min: " SIZE_FORMAT
					"K, Max: " SIZE_FORMAT "K, Ergo: " SIZE_FORMAT "K) -> " SIZE_FORMAT "K",
					thread->name(),
					requested_size * HeapWordSize / K,
					min_size * HeapWordSize / K,
					_max_tlab_size * HeapWordSize / K,
					ergo_tlab * HeapWordSize / K,
					size * HeapWordSize / K);
	}

	// All prepared, let's do it!
	HeapWord* res = allocate_or_collect_work(size);

	if (res != NULL) {
		// Allocation successful
		*actual_size = size;
	}

	return res;
}

HeapWord* SimpleGCHeap::mem_allocate(size_t size,
		bool *gc_overhead_limit_was_exceeded) {
	*gc_overhead_limit_was_exceeded = false;
	return allocate_or_collect_work(size);
}

void SimpleGCHeap::collect(GCCause::Cause cause) {
	switch (cause) {
	case GCCause::_metadata_GC_threshold:
	case GCCause::_metadata_GC_clear_soft_refs:
		// Receiving these causes means the VM itself entered the safepoint for metadata collection.
		// While Epsilon does not do GC, it has to perform sizing adjustments, otherwise we would
		// re-enter the safepoint again very soon.

		assert(SafepointSynchronize::is_at_safepoint(), "Expected at safepoint");
		log_info(gc)("GC request for \"%s\" is handled",
							GCCause::to_string(cause));
		MetaspaceGC::compute_new_size();
		print_metaspace_info();
		break;
	default:
		if (SafepointSynchronize::is_at_safepoint()) {
			entry_collect(cause);
		} else {
			vmentry_collect(cause);
		}
	}
	_monitoring_support->update_counters();
}

void SimpleGCHeap::do_full_collection(bool clear_all_soft_refs) {
	collect(gc_cause());
}

void SimpleGCHeap::safe_object_iterate(ObjectClosure *cl) {
 _space->safe_object_iterate(cl);
}

void SimpleGCHeap::print_on(outputStream *st) const {
	st->print_cr("SimpleGC Heap");

	// Cast away constness:
	((VirtualSpace) _virtual_space).print_on(st);

	st->print_cr("Allocation space:");
	_space->print_on(st);

	MetaspaceUtils::print_on(st);
}

void SimpleGCHeap::print_tracing_info() const {
	print_heap_info(used());
	print_metaspace_info();
}

void SimpleGCHeap::print_heap_info(size_t used) const {
	size_t reserved = max_capacity();
	size_t committed = capacity();

	if (reserved != 0) {
		log_info(gc)("Heap: " SIZE_FORMAT "%s reserved, " SIZE_FORMAT "%s (%.2f%%) committed, "
				SIZE_FORMAT "%s (%.2f%%) used",
				byte_size_in_proper_unit(reserved), proper_unit_for_byte_size(reserved),
				byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
				committed * 100.0 / reserved,
				byte_size_in_proper_unit(used), proper_unit_for_byte_size(used),
				used * 100.0 / reserved);
	} else {
		log_info(gc)("Heap: no reliable data");
	}
}

void SimpleGCHeap::print_metaspace_info() const {
	size_t reserved = MetaspaceUtils::reserved_bytes();
	size_t committed = MetaspaceUtils::committed_bytes();
	size_t used = MetaspaceUtils::used_bytes();

	if (reserved != 0) {
		log_info(gc, metaspace)("Metaspace: " SIZE_FORMAT "%s reserved, " SIZE_FORMAT "%s (%.2f%%) committed, "
			SIZE_FORMAT "%s (%.2f%%) used",
			byte_size_in_proper_unit(reserved), proper_unit_for_byte_size(reserved),
			byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
			committed * 100.0 / reserved,
			byte_size_in_proper_unit(used), proper_unit_for_byte_size(used),
			used * 100.0 / reserved);
	} else {
		log_info(gc, metaspace)("Metaspace: no reliable data");
	}
}

// ------------------ EXPERIMENTAL MARK-COMPACT -------------------------------
//
// This implements a trivial Lisp2-style sliding collector:
//     https://en.wikipedia.org/wiki/Mark-compact_algorithm#LISP2_algorithm
//
// The goal for this implementation is to be as simple as possible, ignoring
// non-trivial performance optimizations. This collector does not implement
// reference processing: no soft/weak/phantom/finalizeable references are ever
// cleared. It also does not implement class unloading and other runtime
// cleanups.
//

// VM operation that executes collection cycle under safepoint
class VM_SimpleGCCollect: public VM_Operation {
	private:
		const GCCause::Cause _cause;
		SimpleGCHeap* const _heap;
		static size_t _last_used;
	public:
		VM_SimpleGCCollect(GCCause::Cause cause) :
		VM_Operation(), _cause(cause), _heap(SimpleGCHeap::heap()) {}
		VM_Operation::VMOp_Type type() const {	return VMOp_SimpleGCCollect;	}
		const char* name() const {	 return "SimpleGCHeap Collection";}

	virtual bool doit_prologue() {
		// Need to take the Heap lock before managing backing storage.
		// This also naturally serializes GC requests, and allows us to coalesce
		// back-to-back allocation failure requests from many threads. There is no
		// need to handle allocation failure that comes without allocations since
		// last complete GC. Waiting for 1% of heap allocated before starting next
		// GC seems to resolve most races.
		Heap_lock->lock();
		size_t used = _heap->used();
		size_t capacity = _heap->capacity();
		size_t allocated = used > _last_used ? used - _last_used : 0;
		if (_cause != GCCause::_allocation_failure || allocated > capacity / 100) {
		      return true;
		} else {
		    Heap_lock->unlock();
		    return false;
		}
	}

	virtual void doit() {
		_heap->entry_collect(_cause);
	}

	virtual void doit_epilogue() {
		_last_used = _heap->used();
		Heap_lock->unlock();
	}
};

size_t VM_SimpleGCCollect::_last_used = 0;

void SimpleGCHeap::vmentry_collect(GCCause::Cause cause) {
	VM_SimpleGCCollect vmop(cause);
	VMThread::execute(&vmop);
}

HeapWord* SimpleGCHeap::allocate_or_collect_work(size_t size) {
	HeapWord* res = allocate_work(size);
	if (res == NULL) {
		vmentry_collect(GCCause::_allocation_failure);
		res = allocate_work(size);
	}
	return res;
}

typedef Stack<oop, mtGC> SimpleGCMarkStack;

void SimpleGCHeap::do_roots(OopClosure* cl, bool everything) {
	// Need to tell runtime we are about to walk the roots with 1 thread
	StrongRootsScope scope(1);

	// Need to adapt oop closure for some special root types.
	CLDToOopClosure clds(cl, ClassLoaderData::_claim_none);
	MarkingCodeBlobClosure blobs(cl, CodeBlobToOopClosure::FixRelocations);

	// Walk all these different parts of runtime roots. Some roots require
	// holding the lock when walking them.
	{
		MutexLocker lock(CodeCache_lock, Mutex::_no_safepoint_check_flag);
		CodeCache::blobs_do(&blobs);
	}
	{
		MutexLocker lock(ClassLoaderDataGraph_lock);
		ClassLoaderDataGraph::cld_do(&clds);
	}
	Universe::oops_do(cl);
	Management::oops_do(cl);
	JvmtiExport::oops_do(cl);
	JNIHandles::oops_do(cl);
	WeakProcessor::oops_do(cl);
	ObjectSynchronizer::oops_do(cl);
	SystemDictionary::oops_do(cl);
	Threads::possibly_parallel_oops_do(false, cl, &blobs);

		// This is implicitly handled by other roots, and we only want to
		// touch these during verification.
	if (everything) {
		StringTable::oops_do(cl);
	}
}

// Walk the marking bitmap and call object closure on every marked object.
// This is much faster that walking a (very sparse) parsable heap, but it
// takes up to 1/64-th of heap size for the bitmap.
void SimpleGCHeap::walk_bitmap(ObjectClosure* cl) {
	HeapWord* limit = _space->top();
	HeapWord* addr = _bitmap.get_next_marked_addr(_space->bottom(), limit);
	while (addr < limit) {
		oop obj = oop(addr);
		assert(_bitmap.is_marked(obj), "sanity");
		cl->do_object(obj);
		addr += 1;
		if (addr < limit) {
			addr = _bitmap.get_next_marked_addr(addr, limit);
		}
	}
}

class ScanOopClosure: public BasicOopIterateClosure {
	private:
		SimpleGCMarkStack* const _stack;
		MarkBitMap* const _bitmap;

		template<class T>
		void do_oop_work(T* p) {
			  // p is the pointer to memory location where oop is, load the value
			  // from it, unpack the compressed reference, if needed:
			T o = RawAccess<>::oop_load(p);
			if (!CompressedOops::is_null(o)) {
				oop obj = CompressedOops::decode_not_null(o);

				// Object is discovered. See if it is marked already. If not,
				// mark and push it on mark stack for further traversal. Non-atomic
				// check and set would do, as this closure is called by single thread.
				if (!_bitmap->is_marked(obj)) {
					_bitmap->mark((HeapWord*) obj);
					_stack->push(obj);
				}
			}
		}

	public:
		ScanOopClosure(SimpleGCMarkStack* stack, MarkBitMap* bitmap) :
			_stack(stack), _bitmap(bitmap) {
		}
		virtual void do_oop(oop* p) {
			do_oop_work(p);
		}
		virtual void do_oop(narrowOop* p) {
			do_oop_work(p);
		}
};

class CalculateNewLocationObjectClosure: public ObjectClosure {
	private:
		HeapWord* _compact_point;
		PreservedMarks* const _preserved_marks;

	public:
		CalculateNewLocationObjectClosure(HeapWord* start, PreservedMarks* pm) :_compact_point(start), _preserved_marks(pm) {}

		void do_object(oop obj) {
			  // Record the new location of the object: it is current compaction point.
			  // If object stays at the same location (which is true for objects in
			  // dense prefix, that we would normally get), do not bother recording the
			  // move, letting downstream code ignore it.
			if ((HeapWord*) obj != _compact_point) {
				markOop mark = obj->mark_raw();
				if (mark->must_be_preserved(obj)) {
					_preserved_marks->push(obj, mark);
				}
				obj->forward_to(oop(_compact_point));
			}
			_compact_point += obj->size();
		}

		HeapWord* compact_point() {
			return _compact_point;
		}
};

class AdjustPointersOopClosure: public BasicOopIterateClosure {
	private:
		template<class T>
		void do_oop_work(T* p) {
				  // p is the pointer to memory location where oop is, load the value
				  // from it, unpack the compressed reference, if needed:
			T o = RawAccess<>::oop_load(p);
			if (!CompressedOops::is_null(o)) {
				oop obj = CompressedOops::decode_not_null(o);

				// Rewrite the current pointer to the object with its forwardee.
				// Skip the write if update is not needed.
				if (obj->is_forwarded()) {
					oop fwd = obj->forwardee();
					assert(fwd != NULL, "just checking");
					RawAccess<>::oop_store(p, fwd);
				}
			}
		}

	public:
		virtual void do_oop(oop* p) {
			do_oop_work(p);
		}
		virtual void do_oop(narrowOop* p) {
			do_oop_work(p);
		}
};

class AdjustPointersObjectClosure: public ObjectClosure {
	private:
		AdjustPointersOopClosure _cl;
	public:
		void do_object(oop obj) {
			// Apply the updates to all references reachable from current object:
			obj->oop_iterate(&_cl);
		}
};

class MoveObjectsObjectClosure: public ObjectClosure {
	private:
		size_t _moved;
	public:
		MoveObjectsObjectClosure():ObjectClosure(), _moved(0) {}

		void do_object(oop obj) {
				// Copy the object to its new location, if needed. This is final step,
				// so we have to re-initialize its new mark word, dropping the forwardee
				// data from it.
			if (obj->is_forwarded()) {
				oop fwd = obj->forwardee();
				assert(fwd != NULL, "just checking");
				Copy::aligned_conjoint_words((HeapWord*) obj, (HeapWord*) fwd, obj->size());
				fwd->init_mark_raw();
				_moved++;
			 }
		}

		size_t moved() {
			return _moved;
		}
};

class SimpleGCVerifyOopClosure: public BasicOopIterateClosure {
	private:
		SimpleGCHeap* const _heap;
		SimpleGCMarkStack* const _stack;
		MarkBitMap* const _bitmap;

		template<class T>
		void do_oop_work(T* p) {
			T o = RawAccess<>::oop_load(p);
			if (!CompressedOops::is_null(o)) {
				oop obj = CompressedOops::decode_not_null(o);
				if (!_bitmap->is_marked(obj)) {
					_bitmap->mark((HeapWord*) obj);

					guarantee(_heap->is_in(obj), "Is in heap: " PTR_FORMAT, p2i(obj));
					guarantee(oopDesc::is_oop(obj), "Is an object: " PTR_FORMAT, p2i(obj));
					guarantee(!obj->mark()->is_marked(), "Mark is gone: " PTR_FORMAT, p2i(obj));

					_stack->push(obj);
				}
			}
		}

	public:
		SimpleGCVerifyOopClosure(SimpleGCMarkStack* stack, MarkBitMap* bitmap) : _heap(SimpleGCHeap::heap()), _stack(stack), _bitmap(bitmap) {}

		virtual void do_oop(oop* p) {
			do_oop_work(p);
		}
		virtual void do_oop(narrowOop* p) {
			do_oop_work(p);
		}
};

void SimpleGCHeap::entry_collect(GCCause::Cause cause) {
	GCIdMark mark;
	GCTraceTime(Info, gc)time("Lisp2-style Mark-Compact", NULL, cause, true);

		  // Some statistics, for fun and profit:
	size_t stat_reachable_roots = 0;
	size_t stat_reachable_heap = 0;
	size_t stat_moved = 0;
	size_t stat_preserved_marks = 0;

	{
		GCTraceTime(Info, gc)time("Step 0: Prologue", NULL);

			  // Commit marking bitmap memory. There are several upsides of doing this
			  // before the cycle: no memory is taken if GC is not happening, the memory
			  // is "cleared" on first touch, and untouched parts of bitmap are mapped
			  // to zero page, boosting performance on sparse heaps.
		if (!os::commit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size(), false)) {
			log_warning(gc)("Could not commit native memory for marking bitmap, GC failed");
			return;
		}

			// We do not need parsable heap for this algorithm to work, but we want
			// threads to give up their TLABs.
		ensure_parsability(true);

			// Tell various parts of runtime we are doing GC.
		BiasedLocking::preserve_marks();

			// Derived pointers would be re-discovered during the mark.
			// Clear and activate the table for them.
		DerivedPointerTable::clear();
	}

	{
		GCTraceTime(Info, gc)time("Step 1: Mark", NULL);

			// Marking stack and the closure that does most of the work. The closure
			// would scan the outgoing references, mark them, and push newly-marked
			// objects to stack for further processing.
		SimpleGCMarkStack stack;
		ScanOopClosure cl(&stack, &_bitmap);

			// Seed the marking with roots.
		process_roots(&cl);
		stat_reachable_roots = stack.size();

			// Scan the rest of the heap until we run out of objects. Termination is
			// guaranteed, because all reachable objects would be marked eventually.
		while (!stack.is_empty()) {
			oop obj = stack.pop();
			obj->oop_iterate(&cl);
			stat_reachable_heap++;
		}

		// No more derived pointers discovered after marking is done.
		DerivedPointerTable::set_active(false);
	}

		// We are going to store forwarding information (where the new copy resides)
		// in mark words. Some of those mark words need to be carefully preserved.
		// This is an utility that maintains the list of those special mark words.
	PreservedMarks preserved_marks;

		// New top of the allocated space.
	HeapWord* new_top;

	{
		GCTraceTime(Info, gc)time("Step 2: Calculate new locations", NULL);

			// Walk all alive objects, compute their new addresses and store those
			// addresses in mark words. Optionally preserve some marks.
		CalculateNewLocationObjectClosure cl(_space->bottom(), &preserved_marks);
		walk_bitmap(&cl);

			// After addresses are calculated, we know the new top for the allocated
			// space. We cannot set it just yet, because some asserts check that objects
			// are "in heap" based on current "top".
		new_top = cl.compact_point();

		stat_preserved_marks = preserved_marks.size();
	}

	{
		GCTraceTime(Info, gc)time("Step 3: Adjust pointers", NULL);

		  // Walk all alive objects _and their reference fields_, and put "new
		  // addresses" there. We know the new addresses from the forwarding data
		  // in mark words. Take care of the heap objects first.
		AdjustPointersObjectClosure cl;
		walk_bitmap(&cl);

		  // Now do the same, but for all VM roots, which reference the objects on
		  // their own: their references should also be updated.
		AdjustPointersOopClosure cli;
		process_roots(&cli);

		  // Finally, make sure preserved marks know the objects are about to move.
		preserved_marks.adjust_during_full_gc();
	}

	{
		GCTraceTime(Info, gc)time("Step 4: Move objects", NULL);

			// Move all alive objects to their new locations. All the references are
			// already adjusted at previous step.
		MoveObjectsObjectClosure cl;
		walk_bitmap(&cl);
		stat_moved = cl.moved();

			// Now we moved all objects to their relevant locations, we can retract
			// the "top" of the allocation space to the end of the compacted prefix.
		_space->set_top(new_top);
	}

	{
		GCTraceTime(Info, gc)time("Step 5: Epilogue", NULL);

			// Restore all special mark words.
		preserved_marks.restore();

			// Tell the rest of runtime we have finished the GC.
		DerivedPointerTable::update_pointers();
		BiasedLocking::restore_marks();
		JvmtiExport::gc_epilogue();

			// Verification code walks entire heap and verifies nothing is broken.
		bool vefigyHeap = true;
		if (vefigyHeap) {
			// The basic implementation turns heap into entirely parsable one with
			// only alive objects, which mean we could just walked the heap object
			// by object and verify it. But, it would be inconvenient for verification
			// to assume heap has only alive objects. Any future change that leaves
			// at least one dead object with dead outgoing references would fail the
			// verification. Therefore, it makes more sense to mark through the heap
			// again, not assuming objects are all alive.
			SimpleGCMarkStack stack;
			SimpleGCVerifyOopClosure cl(&stack, &_bitmap);

			_bitmap.clear();

			// Verify all roots are correct, and that we have the same number of
			// object reachable from roots.
			process_all_roots(&cl);

			size_t verified_roots = stack.size();
			guarantee(verified_roots == stat_reachable_roots,
					"Verification discovered " SIZE_FORMAT " roots out of " SIZE_FORMAT,
					verified_roots, stat_reachable_roots);

			// Verify the rest of the heap is correct, and that we have the same
			// number of objects reachable from heap.
			size_t verified_heap = 0;
			while (!stack.is_empty()) {
				oop obj = stack.pop();
				obj->oop_iterate(&cl);
				verified_heap++;
			}

			guarantee(verified_heap == stat_reachable_heap,
					"Verification discovered " SIZE_FORMAT " heap objects out of " SIZE_FORMAT,
					verified_heap, stat_reachable_heap);

			// Ask parts of runtime to verify themselves too
			Universe::verify(VerifyOption_Default, "");
		}

			// Marking bitmap is not needed anymore
		if (!os::uncommit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size())) {
			log_warning(gc)("Could not uncommit native memory for marking bitmap");
		}

	}

	size_t stat_reachable = stat_reachable_roots + stat_reachable_heap;
	log_info(gc)("GC Stats: " SIZE_FORMAT " (%.2f%%) reachable from roots, " SIZE_FORMAT " (%.2f%%) reachable from heap, "
	SIZE_FORMAT " (%.2f%%) moved, " SIZE_FORMAT " (%.2f%%) markwords preserved",
	stat_reachable_roots, 100.0 * stat_reachable_roots / stat_reachable,
	stat_reachable_heap, 100.0 * stat_reachable_heap / stat_reachable,
	stat_moved, 100.0 * stat_moved / stat_reachable,
	stat_preserved_marks, 100.0 * stat_preserved_marks / stat_reachable);

	print_heap_info(used());
	print_metaspace_info();
}

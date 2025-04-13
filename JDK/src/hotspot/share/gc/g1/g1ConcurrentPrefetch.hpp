/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_G1CONCURRENTPREFETCH_HPP
#define SHARE_VM_GC_G1_G1CONCURRENTPREFETCH_HPP

#include "gc/g1/g1ConcurrentMarkBitMap.hpp"
#include "gc/g1/g1ConcurrentMarkObjArrayProcessor.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1RegionMarkStatsCache.hpp"
#include "gc/g1/heapRegionSet.hpp"
#include "gc/shared/taskqueue.hpp"
#include "memory/allocation.hpp"



#include "gc/g1/g1ConcurrentMark.hpp"
// #include "gc/g1/g1ConcurrentPrefetchObjArrayProcessor.hpp"


class G1ConcurrentPrefetchThread;
class G1ConcurrentPrefetch;
class G1PFTask;


typedef GenericTaskQueue<G1TaskQueueEntry, mtGC, INSTRUMENT_QUEUE_SIZE> G1InstrumentTaskQueue;
// typedef GenericTaskQueueSet<G1InstrumentTaskQueue, mtGC> G1InstrumentTaskQueueSet;

typedef GenericTaskQueue<G1TaskQueueEntry, mtGC, PREFETCH_QUEUE_SIZE> G1PFTaskQueue;
typedef GenericTaskQueueSet<G1PFTaskQueue, mtGC> G1PFTaskQueueSet;


// class G1ConcurrentInstrumentQueueSet : public CHeapObj<mtGC> {
// public:
//   G1InstrumentTaskQueue** _queues;
//   int* _free_list;
//   uint _free_first;
//   uint _free_last;

//   uint _n;
//   uint _max_threads;

//   G1ConcurrentInstrumentQueueSet(uint n) : _n(n), _max_threads(1024) {
//     _queues = NEW_C_HEAP_ARRAY(G1InstrumentTaskQueue, _max_threads, mtGC);
//     _free_list = NEW_C_HEAP_ARRAY(int, _max_threads, mtGC);
//     for (uint i = 0; i < _max_threads; i++) {
//       _queues[i] = NULL;
//       _free_list[i] = i+1;
//     }
//     _free_list[_max_threads] = -1;
//   }

//   uint register_new_queue() { 
//     G1InstrumentTaskQueue* new_queue = new G1InstrumentTaskQueue();
//     _queues[_first] = 
//     _free_list[_first] = -1;
//     _first = 
//     return 
//   }

//   uint delete_queue() { 
//     uint j = 0;
//     for()
//     return 
//   }


// }



// This class manages data structures and methods for doing liveness analysis in
// G1's concurrent cycle.
class G1ConcurrentPrefetch : public CHeapObj<mtGC> {
  friend class G1ConcurrentMarkThread;
  friend class G1ConcurrentPrefetchThread;
  friend class G1CMRefProcTaskProxy;
  friend class G1CMRefProcTaskExecutor;
  friend class G1CMKeepAliveAndDrainClosure;
  friend class G1CMDrainMarkingStackClosure;
  friend class G1CMBitMapClosure;
  friend class G1CMConcurrentMarkingTask;
//   friend class G1CMRemarkTask;
  friend class G1CMTask;
  friend class G1PFTask;
  friend class G1ConcurrentMark;
  friend class G1PFConcurrentPrefetchingTask;

  size_t                  _current_queue_index;

  G1ConcurrentMark*       _cm;
  G1ConcurrentPrefetchThread* _pf_thread;     // The thread doing the work
  G1CollectedHeap*        _g1h;           // The heap
  bool                    _completed_initialization; // Set to true when initialization is complete

  // Concurrent marking support structures
//   G1CMBitMap              _mark_bitmap_1;
//   G1CMBitMap              _mark_bitmap_2;
//   G1CMBitMap*             _prev_mark_bitmap; // Completed mark bitmap
//   G1CMBitMap*             _next_mark_bitmap; // Under-construction mark bitmap

  // Heap bounds
  MemRegion const         _heap;

  // For grey objects
  G1CMMarkStack*           _global_mark_stack; // Grey objects behind global finger
  // HeapWord* volatile      _finger;            // The global finger, region aligned,
                                              // always pointing to the end of the
                                              // last claimed region

  uint                    _worker_id_offset;
  uint                    _max_num_tasks;    // Maximum number of marking tasks
  uint                    _num_active_tasks; // Number of tasks currently active
  G1PFTask**              _tasks;            // Task queue array (max_worker_id length)

  G1PFTaskQueueSet*       _task_queues; // Task queue set
  // TaskTerminator          _terminator;  // For termination
public:  
  // Haoran: TODO
  // G1InstrumentTaskQueue* _instrument_queue;
private:
  // Two sync barriers that are used to synchronize tasks when an
  // overflow occurs. The algorithm is the following. All tasks enter
  // the first one to ensure that they have all stopped manipulating
  // the global data structures. After they exit it, they re-initialize
  // their data structures and task 0 re-initializes the global data
  // structures. Then, they enter the second sync barrier. This
  // ensure, that no task starts doing work before all data
  // structures (local and global) have been re-initialized. When they
  // exit it, they are free to start working again.
  // WorkGangBarrierSync     _first_overflow_barrier_sync;
  // WorkGangBarrierSync     _second_overflow_barrier_sync;

  // This is set by any task, when an overflow on the global data
  // structures is detected
  // volatile bool           _has_overflown;
  // True: marking is concurrent, false: we're in remark
  volatile bool           _concurrent;
  // Set at the end of a Full GC so that marking aborts
  volatile bool           _has_aborted;

  // Used when remark aborts due to an overflow to indicate that
  // another concurrent marking phase should start
  // volatile bool           _restart_for_overflow;

  // ConcurrentGCTimer*      _gc_timer_cm;

  // G1OldTracer*            _gc_tracer_cm;

  // Timing statistics. All of them are in ms
  // NumberSeq _init_times;
//   NumberSeq _remark_times;
//   NumberSeq _remark_mark_times;
//   NumberSeq _remark_weak_ref_times;
//   NumberSeq _cleanup_times;
//   double    _total_cleanup_time;

  double*   _accum_task_vtime;   // Accumulated task vtime

  WorkGang* _concurrent_workers;
  uint      _num_concurrent_workers; // The number of marking worker threads we're using
  uint      _max_concurrent_workers; // Maximum number of marking worker threads

//   void verify_during_pause(G1HeapVerifier::G1VerifyType type, VerifyOption vo, const char* caller);

//   void finalize_marking();

//   void weak_refs_work_parallel_part(BoolObjectClosure* is_alive, bool purged_classes);
//   void weak_refs_work(bool clear_all_soft_refs);

//   void report_object_count(bool mark_completed);

//   void swap_mark_bitmaps();

//   void reclaim_empty_regions();

  // After reclaiming empty regions, update heap sizes.
//   void compute_new_sizes();

  // Clear statistics gathered during the concurrent cycle for the given region after
  // it has been reclaimed.
//   void clear_statistics(HeapRegion* r);

  // Resets the global marking data structures, as well as the
  // task local ones; should be called during initial mark.
  void reset();

  // Resets all the marking data structures. Called when we have to restart
  // marking or when marking completes (via set_non_marking_state below).
  void reset_marking_for_restart();

  // We do this after we're done with marking so that the marking data
  // structures are initialized to a sensible and predictable state.
  void reset_at_marking_complete();

  // Called to indicate how many threads are currently active.
  void set_concurrency(uint active_tasks);

  // Should be called to indicate which phase we're in (concurrent
  // mark or remark) and how many threads are currently active.
  void set_concurrency_and_phase(uint active_tasks, bool concurrent);

//   // Prints all gathered CM-related statistics
//   void print_stats();

// //   HeapWord*               finger()           { return _finger;   }
//   bool                    concurrent()       { return _concurrent; }
//   uint                    active_tasks()     { return _num_active_tasks; }
//   ParallelTaskTerminator* terminator() const { return _terminator.terminator(); }

//   // Claims the next available region to be scanned by a marking
//   // task/thread. It might return NULL if the next region is empty or
//   // we have run out of regions. In the latter case, out_of_regions()
//   // determines whether we've really run out of regions or the task
//   // should call claim_region() again. This might seem a bit
//   // awkward. Originally, the code was written so that claim_region()
//   // either successfully returned with a non-empty region or there
//   // were no more regions to be claimed. The problem with this was
//   // that, in certain circumstances, it iterated over large chunks of
//   // the heap finding only empty regions and, while it was working, it
//   // was preventing the calling task to call its regular clock
//   // method. So, this way, each task will spend very little time in
//   // claim_region() and is allowed to call the regular clock method
//   // frequently.
// //   HeapRegion* claim_region(uint worker_id);

//   // Determines whether we've run out of regions to scan. Note that
//   // the finger can point past the heap end in case the heap was expanded
//   // to satisfy an allocation without doing a GC. This is fine, because all
//   // objects in those regions will be considered live anyway because of
//   // SATB guarantees (i.e. their TAMS will be equal to bottom).
//   bool out_of_regions() { return _finger >= _heap.end(); }

  // Returns the task with the given id
  G1PFTask* task(uint id) {
    // During initial mark we use the parallel gc threads to do some work, so
    // we can only compare against _max_num_tasks.
    assert(id < _max_num_tasks, "Task id %u not within bounds up to %u", id, _max_num_tasks);
    return _tasks[id];
  }

//   // Access / manipulation of the overflow flag which is set to
//   // indicate that the global stack has overflown
//   bool has_overflown()           { return _has_overflown; }
//   void set_has_overflown()       { _has_overflown = true; }
//   void clear_has_overflown()     { _has_overflown = false; }
//   bool restart_for_overflow()    { return _restart_for_overflow; }

//   Methods to enter the two overflow sync barriers
//   void enter_first_sync_barrier(uint worker_id);
//   void enter_second_sync_barrier(uint worker_id);

  // Clear the given bitmap in parallel using the given WorkGang. If may_yield is
  // true, periodically insert checks to see if this method should exit prematurely.
  // void clear_bitmap(G1CMBitMap* bitmap, WorkGang* workers, bool may_yield);

  // Region statistics gathered during marking.
  G1RegionMarkStats* _region_mark_stats;
  // Top pointer for each region at the start of the rebuild remembered set process
  // for regions which remembered sets need to be rebuilt. A NULL for a given region
  // means that this region does not be scanned during the rebuilding remembered
  // set phase at all.
  // HeapWord* volatile* _top_at_rebuild_starts;
public:

  size_t prefetch_queue_index() { 
    _current_queue_index ++;
    JavaThreadIteratorWithHandle jtiwh;
    size_t length = (size_t) jtiwh.length();
    _current_queue_index %= length;
    return _current_queue_index;
  }

  void add_to_liveness(uint worker_id, oop const obj, size_t size);
  // Liveness of the given region as determined by concurrent marking, i.e. the amount of
  // live words between bottom and nTAMS.
//   size_t liveness(uint region) const { return _region_mark_stats[region]._live_words; }

//   // Sets the internal top_at_region_start for the given region to current top of the region.
//   // inline void update_top_at_rebuild_start(HeapRegion* r);
//   // TARS for the given region during remembered set rebuilding.
//   // inline HeapWord* top_at_rebuild_start(uint region) const;

  // Clear statistics gathered during the concurrent cycle for the given region after
  // it has been reclaimed.
  void clear_statistics_in_region(uint region_idx);
//   // Notification for eagerly reclaimed regions to clean up.
//   // void humongous_object_eagerly_reclaimed(HeapRegion* r);
//   // Manipulation of the global mark stack.
//   // The push and pop operations are used by tasks for transfers
//   // between task-local queues and the global mark stack.
//   // bool mark_stack_push(G1TaskQueueEntry* arr) {
//   //   if (!_global_mark_stack.par_push_chunk(arr)) {
//   //     set_has_overflown();
//   //     return false;
//   //   }
//   //   return true;
//   // }
//   // bool mark_stack_pop(G1TaskQueueEntry* arr) {
//   //   return _global_mark_stack.par_pop_chunk(arr);
//   // }
//   // size_t mark_stack_size() const                { return _global_mark_stack.size(); }
//   // size_t partial_mark_stack_size_target() const { return _global_mark_stack.capacity() / 3; }
//   // bool mark_stack_empty() const                 { return _global_mark_stack.is_empty(); }

//   // G1CMRootRegions* root_regions() { return &_root_regions; }

//   // void concurrent_cycle_start();
//   // Abandon current marking iteration due to a Full GC.
//   void concurrent_cycle_abort();
//   void concurrent_cycle_end();

  void update_accum_task_vtime(int i, double vtime) {
    _accum_task_vtime[i] += vtime;
  }

//   // double all_task_accum_vtime() {
//   //   double ret = 0.0;
//   //   for (uint i = 0; i < _max_num_tasks; ++i)
//   //     ret += _accum_task_vtime[i];
//   //   return ret;
//   // }

//   // Attempts to steal an object from the task queues of other tasks
//   bool try_stealing(uint worker_id, G1TaskQueueEntry& task_entry);

  G1ConcurrentPrefetch(G1CollectedHeap* g1h, G1ConcurrentMark* cm);
  ~G1ConcurrentPrefetch();

  G1ConcurrentPrefetchThread* pf_thread() { return _pf_thread; }

//   // const G1CMBitMap* const prev_mark_bitmap() const { return _prev_mark_bitmap; }
//   // G1CMBitMap* next_mark_bitmap() const { return _next_mark_bitmap; }

//   // Calculates the number of concurrent GC threads to be used in the marking phase.
//   uint calc_active_marking_workers();

  // Moves all per-task cached data into global state.
  void flush_all_task_caches();
//   // Prepare internal data structures for the next mark cycle. This includes clearing
//   // the next mark bitmap and some internal data structures. This method is intended
//   // to be called concurrently to the mutator. It will yield to safepoint requests.
//   // void cleanup_for_next_mark();

//   // Clear the previous marking bitmap during safepoint.
//   // void clear_prev_bitmap(WorkGang* workers);

//   // These two methods do the work that needs to be done at the start and end of the
//   // initial mark pause.
  void pre_initial_mark();
//   // void post_initial_mark();

//   // Scan all the root regions and mark everything reachable from
//   // them.
//   // void scan_root_regions();

//   // Scan a single root region from nTAMS to top and mark everything reachable from it.
//   // void scan_root_region(HeapRegion* hr, uint worker_id);

  // Do concurrent phase of marking, to a tentative transitive closure.
  void mark_from_stacks();

//   // Do concurrent preclean work.
//   // void preclean();

//   // void remark();

  // void cleanup();
  // Mark in the previous bitmap. Caution: the prev bitmap is usually read-only, so use
  // this carefully.
  inline void mark_in_prev_bitmap(oop p);

  inline bool is_marked_in_prev_bitmap(oop p) const;


  inline bool do_yield_check();

  bool has_aborted()            { return _has_aborted; }
  void set_has_aborted()        { _has_aborted = true; }
  void clear_has_aborted()      { _has_aborted = false; }

//   void print_summary_info();

//   void print_worker_threads_on(outputStream* st) const;
//   void threads_do(ThreadClosure* tc) const;

//   void print_on_error(outputStream* st) const;

  // Mark the given object on the next bitmap if it is below nTAMS.
  inline bool mark_in_next_bitmap(uint worker_id, HeapRegion* const hr, oop const obj);
  inline bool mark_in_next_bitmap(uint worker_id, oop const obj);
  inline bool mark_prefetch_in_next_bitmap(uint worker_id, oop const obj, G1PFTask* task);
  inline bool mark_black_in_next_bitmap(uint worker_id, oop const obj);

  inline bool is_below_global_finger(oop obj) const;

  inline bool is_marked_in_next_bitmap(oop p) const;

//   // Returns true if initialization was successfully completed.
//   bool completed_initialization() const {
//     return _completed_initialization;
//   }

//   // ConcurrentGCTimer* gc_timer_cm() const { return _gc_timer_cm; }
//   // G1OldTracer* gc_tracer_cm() const { return _gc_tracer_cm; }

// private:
//   // Rebuilds the remembered sets for chosen regions in parallel and concurrently to the application.
//   // void rebuild_rem_set_concurrently();
};

// A class representing a marking task.
class G1PFTask : public TerminatorTerminator {
  friend class G1PFConcurrentPrefetchingTask;
  friend class G1ConcurrentPrefetch;
private:
  enum PrivateConstants {
    // The regular clock call is called once the scanned words reaches
    // this limit
    words_scanned_period          = 12*1024,
    // The regular clock call is called once the number of visited
    // references reaches this limit
    refs_reached_period           = 1024,
    // Initial value for the hash seed, used in the work stealing code
    init_hash_seed                = 17
  };

  // Number of entries in the per-task stats entry. This seems enough to have a very
  // low cache miss rate.
  static const uint RegionMarkStatsCacheSize = 1024;

  G1PFObjArrayProcessor       _objArray_processor;

  uint                        _worker_id;
  G1CollectedHeap*            _g1h;
  G1ConcurrentMark*           _cm;
  G1ConcurrentPrefetch*       _pf;
  G1CMBitMap*                 _next_mark_bitmap;
  G1CMBitMap*                 _next_black_mark_bitmap;
  // the task queue of this task
  G1PFTaskQueue*              _task_queue;

  G1RegionMarkStatsCache      _mark_stats_cache;
  // Number of calls to this task
  uint                        _calls;

  // When the virtual timer reaches this time, the marking step should exit
  double                      _time_target_ms;
  // Start time of the current marking step
  double                      _start_time_ms;

  // Oop closure used for iterations over oops
  G1PFOopClosure*             _cm_oop_closure;

  // Region this task is scanning, NULL if we're not scanning any
  HeapRegion*                 _curr_region;
  // Local finger of this task, NULL if we're not scanning a region
  HeapWord*                   _finger;
  // Limit of the region this task is scanning, NULL if we're not scanning one
  HeapWord*                   _region_limit;

  // Number of words this task has scanned
  size_t                      _words_scanned;
  size_t                      _objs_scanned;
  // When _words_scanned reaches this limit, the regular clock is
  // called. Notice that this might be decreased under certain
  // circumstances (i.e. when we believe that we did an expensive
  // operation).
  size_t                      _words_scanned_limit;
  // Initial value of _words_scanned_limit (i.e. what it was
  // before it was decreased).
  size_t                      _real_words_scanned_limit;

  // Number of references this task has visited
  size_t                      _refs_reached;
  // When _refs_reached reaches this limit, the regular clock is
  // called. Notice this this might be decreased under certain
  // circumstances (i.e. when we believe that we did an expensive
  // operation).
  size_t                      _refs_reached_limit;
  // Initial value of _refs_reached_limit (i.e. what it was before
  // it was decreased).
  size_t                      _real_refs_reached_limit;

  // If true, then the task has aborted for some reason
  bool                        _has_aborted;
  // Set when the task aborts because it has met its time quota
  bool                        _has_timed_out;
  // True when we're draining SATB buffers; this avoids the task
  // aborting due to SATB buffers being available (as we're already
  // dealing with them)
  bool                        _draining_satb_buffers;

  // Number sequence of past step times
  NumberSeq                   _step_times_ms;
  // Elapsed time of this task
  double                      _elapsed_time_ms;
  // Termination time of this task
  double                      _termination_time_ms;
  // When this task got into the termination protocol
  double                      _termination_start_time_ms;

  TruncatedSeq                _marking_step_diffs_ms;

  uint _count_local_queue_page_local;
  uint _count_local_queue_page_remote;

  uint _count_prefetch_white;
  uint _count_prefetch_grey;
  uint _count_prefetch_black;

  uint _count_steal;

//   // Updates the local fields after this task has claimed
//   // a new region to scan
//   void setup_for_region(HeapRegion* hr);
//   // Makes the limit of the region up-to-date
//   void update_region_limit();

//   // Called when either the words scanned or the refs visited limit
//   // has been reached
//   void reached_limit();
//   // Recalculates the words scanned and refs visited limits
//   void recalculate_limits();
//   // Decreases the words scanned and refs visited limits when we reach
//   // an expensive operation
//   void decrease_limits();
//   // Checks whether the words scanned or refs visited reached their
//   // respective limit and calls reached_limit() if they have
//   void check_limits() {
//     if (_words_scanned >= _words_scanned_limit ||
//         _refs_reached >= _refs_reached_limit) {
//       reached_limit();
//     }
//   }
//   // Supposed to be called regularly during a marking step as
//   // it checks a bunch of conditions that might cause the marking step
//   // to abort
//   // Return true if the marking step should continue. Otherwise, return false to abort
//   bool regular_clock_call();

//   // Set abort flag if regular_clock_call() check fails
//   inline void abort_marking_if_regular_check_fail();

//   // Test whether obj might have already been passed over by the
//   // mark bitmap scan, and so needs to be pushed onto the mark stack.
//   bool is_below_finger(oop obj, HeapWord* global_finger) const;

  template<bool scan> void process_grey_task_entry(G1TaskQueueEntry task_entry);
public:
  // Apply the closure on the given area of the objArray. Return the number of words
  // scanned.
  inline size_t scan_objArray(objArrayOop obj, MemRegion mr);
  // Resets the task; should be called right at the beginning of a marking phase.
  void reset(G1CMBitMap* next_mark_bitmap, G1CMBitMap* next_black_mark_bitmap);
  // // Clears all the fields that correspond to a claimed region.
  // void clear_region_fields();

  // The main method of this class which performs a marking step
  // trying not to exceed the given duration. However, it might exit
  // prematurely, according to some conditions (i.e. SATB buffers are
  // available for processing).
  void do_marking_step();
  // void do_marking_step(double target_ms, bool do_termination, bool is_serial);

  // These two calls start and stop the timer
  void record_start_time() {
    _elapsed_time_ms = os::elapsedTime() * 1000.0;
  }
  void record_end_time() {
    _elapsed_time_ms = os::elapsedTime() * 1000.0 - _elapsed_time_ms;
  }

//   // Returns the worker ID associated with this task.
//   uint worker_id() { return _worker_id; }

  // From TerminatorTerminator. It determines whether this task should
  // exit the termination protocol after it's entered it.
  virtual bool should_exit_termination();

//   // Resets the local region fields after a task has finished scanning a
//   // region; or when they have become stale as a result of the region
//   // being evacuated.
//   void giveup_current_region();

//   HeapWord* finger()            { return _finger; }

  bool has_aborted()            { return _has_aborted; }
  void set_has_aborted()        { _has_aborted = true; }
  void clear_has_aborted()      { _has_aborted = false; }

  void set_cm_oop_closure(G1PFOopClosure* cm_oop_closure);

  // Increment the number of references this task has visited.
  void increment_refs_reached() { ++_refs_reached; }

  // Grey the object by marking it.  If not already marked, push it on
  // the local queue if below the finger. obj is required to be below its region's NTAMS.
  // Returns whether there has been a mark to the bitmap.
  inline bool make_reference_grey(oop obj);
  inline bool make_reference_black(oop obj);
  inline bool make_prefetch_reference_black(oop obj);

  // Grey the object (by calling make_grey_reference) if required,
  // e.g. obj is below its containing region's NTAMS.
  // Precondition: obj is a valid heap object.
  // Returns true if the reference caused a mark to be set in the next bitmap.
  template <class T>
  inline bool deal_with_reference(T* p);

  // Scans an object and visits its children.
  inline void scan_task_entry(G1TaskQueueEntry task_entry);

  // Pushes an object on the local queue.
  inline void push(G1TaskQueueEntry task_entry);

  // Move entries to the global stack.
  void move_entries_to_global_stack();
//   // Move entries from the global stack, return true if we were successful to do so.
//   bool get_entries_from_global_stack();

//   // Pops and scans objects from the local queue. If partially is
//   // true, then it stops when the queue size is of a given limit. If
//   // partially is false, then it stops when the queue is empty.
  void drain_local_queue(bool partially);
//   // Moves entries from the global stack to the local queue and
//   // drains the local queue. If partially is true, then it stops when
//   // both the global stack and the local queue reach a given size. If
//   // partially if false, it tries to empty them totally.
//   void drain_global_stack(bool partially);
//   // Keeps picking SATB buffers and processing them until no SATB
//   // buffers are available.
//   void drain_satb_buffers();

//   // Moves the local finger to a new location
//   inline void move_finger_to(HeapWord* new_finger) {
//     assert(new_finger >= _finger && new_finger < _region_limit, "invariant");
//     _finger = new_finger;
//   }

  G1PFTask(uint worker_id,
           G1ConcurrentMark *cm,
           G1ConcurrentPrefetch *pf,
           G1PFTaskQueue* task_queue,
           G1RegionMarkStats* mark_stats,
           uint max_regions);

  inline void update_liveness(oop const obj, size_t const obj_size);

  // Clear (without flushing) the mark cache entry for the given region.
  void clear_mark_stats_cache(uint region_idx);
  // Evict the whole statistics cache into the global statistics. Returns the
  // number of cache hits and misses so far.
  Pair<size_t, size_t> flush_mark_stats_cache();
//   // Prints statistics associated with this task
//   void print_stats();
  void clear_memliner_stats(){
    _count_local_queue_page_local = 0;
    _count_local_queue_page_remote = 0;
    _count_prefetch_black = 0;
    _count_prefetch_grey = 0;
    _count_prefetch_white = 0;
    _count_steal = 0;
  }

  void print_memliner_stats(){  
    log_info(gc)(
      "prefetcher _count_local_queue_page_local: %u _count_local_queue_page_remote: %u _count_steal: %u",
      _count_local_queue_page_local, _count_local_queue_page_remote, _count_steal
    );
    log_info(gc)(
      "prefetcher _count_prefetch_black: %u _count_prefetch_grey: %u _count_prefetch_white: %u",
      _count_prefetch_black, _count_prefetch_grey, _count_prefetch_white
    );
  }
};


#endif // SHARE_VM_GC_G1_G1CONCURRENTPREFETCH_HPP

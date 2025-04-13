/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RegionMarkStatsCache.inline.hpp"
#include "gc/g1/g1StringDedup.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/genOopClosures.inline.hpp"
#include "gc/shared/referencePolicy.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "include/jvm.h"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "oops/access.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/prefetch.inline.hpp"
#include "services/memTracker.hpp"
#include "utilities/align.hpp"
#include "utilities/growableArray.hpp"



#include "gc/g1/g1ConcurrentPrefetch.inline.hpp"
#include "gc/g1/g1ConcurrentPrefetchThread.inline.hpp"


// // Returns the maximum number of workers to be used in a concurrent
// // phase based on the number of GC workers being used in a STW
// // phase.
// static uint scale_concurrent_worker_threads(uint num_gc_workers) {
//   return MAX2((num_gc_workers + 2) / 4, 1U);
// }

G1ConcurrentPrefetch::G1ConcurrentPrefetch(G1CollectedHeap* g1h, G1ConcurrentMark* cm) :
  _current_queue_index(0),
  // _cm_thread set inside the constructor
  _cm(cm),
  _g1h(g1h),
  _completed_initialization(false),
  // _mark_bitmap_1(),
  // _mark_bitmap_2(),
  // _prev_mark_bitmap(&_mark_bitmap_1),
  // _next_mark_bitmap(&_mark_bitmap_2),
  _heap(_g1h->reserved_region()),
  // _root_regions(_g1h->max_regions()),
  _global_mark_stack(&(cm->_global_mark_stack)),
  // _finger set in set_non_marking_state
  _worker_id_offset(DirtyCardQueueSet::num_par_ids() + G1ConcRefinementThreads + ConcGCThreads),
  _max_num_tasks(ParallelGCThreads),
  // _num_active_tasks set in set_non_marking_state()
  // _tasks set inside the constructor
  _task_queues(new G1PFTaskQueueSet((int) _max_num_tasks)),
  // _instrument_queue(new G1InstrumentTaskQueue()),
  // _terminator((int) _max_num_tasks, _task_queues),
  // _first_overflow_barrier_sync(),
  // _second_overflow_barrier_sync(),
  // _has_overflown(false),
  _concurrent(false),
  _has_aborted(false),
  // _restart_for_overflow(false),
  // _gc_timer_cm(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
  // _gc_tracer_cm(new (ResourceObj::C_HEAP, mtGC) G1OldTracer()),
  // _verbose_level set below
  // _init_times(),
  // _remark_times(),
  // _remark_mark_times(),
  // _remark_weak_ref_times(),
  // _cleanup_times(),
  // _total_cleanup_time(0.0),

  _accum_task_vtime(NULL),

  _concurrent_workers(NULL),
  _num_concurrent_workers(0),
  _max_concurrent_workers(0),

  _region_mark_stats(cm->_region_mark_stats)
  // _top_at_rebuild_starts(NEW_C_HEAP_ARRAY(HeapWord*, _g1h->max_regions(), mtGC))
{
  // _mark_bitmap_1.initialize(g1h->reserved_region(), prev_bitmap_storage);
  // _mark_bitmap_2.initialize(g1h->reserved_region(), next_bitmap_storage);

  // Create & start ConcurrentMark thread.
  _pf_thread = new G1ConcurrentPrefetchThread(this, cm);
  if (_pf_thread->osthread() == NULL) {
    vm_shutdown_during_initialization("Could not create ConcurrentMarkThread");
  }

  assert(CPF_lock != NULL, "CPF_lock must be initialized");

  // if (FLAG_IS_DEFAULT(PrefetchThreads) || PrefetchThreads == 0) {
  if (PrefetchThreads == 0) {
    ShouldNotReachHere();
    // Calculate the number of concurrent worker threads by scaling
    // the number of parallel GC threads.
    // uint marking_thread_num = scale_concurrent_worker_threads(ParallelGCThreads);
    // FLAG_SET_ERGO(uint, ConcGCThreads, marking_thread_num);
  }

  assert(PrefetchThreads > 0, "PrefetchThreads have been set.");

  log_debug(gc)("ConcGCThreads: %u offset %u", ConcGCThreads, _worker_id_offset);
  log_debug(gc)("ParallelGCThreads: %u", ParallelGCThreads);

  _num_concurrent_workers = PrefetchThreads;
  _max_concurrent_workers = _num_concurrent_workers;

  _concurrent_workers = new WorkGang("G1 Pref", _max_concurrent_workers, false, true);
  _concurrent_workers->initialize_workers();

  _tasks = NEW_C_HEAP_ARRAY(G1PFTask*, _max_num_tasks, mtGC);
  _accum_task_vtime = NEW_C_HEAP_ARRAY(double, _max_num_tasks, mtGC);

  // so that the assertion in MarkingTaskQueue::task_queue doesn't fail
  _num_active_tasks = _max_num_tasks;

  for (uint i = 0; i < _max_num_tasks; ++i) {
    G1PFTaskQueue* task_queue = new G1PFTaskQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);

    _tasks[i] = new G1PFTask(i, cm, this, task_queue, _region_mark_stats, _g1h->max_regions());

    _accum_task_vtime[i] = 0.0;
  }

  reset_at_marking_complete();
  _completed_initialization = true;
}

void G1ConcurrentPrefetch::reset() {
  _has_aborted = false;

  reset_marking_for_restart();

  // Reset all tasks, since different phases will use different number of active
  // threads. So, it's easiest to have all of them ready.
  for (uint i = 0; i < _max_num_tasks; ++i) {
    _tasks[i]->reset(_cm->next_mark_bitmap(), _cm->next_black_mark_bitmap());
  }

  // uint max_regions = _g1h->max_regions();
  // for (uint i = 0; i < max_regions; i++) {
  //   _top_at_rebuild_starts[i] = NULL;
  //   _region_mark_stats[i].clear();
  // }
}

bool G1PFTask::should_exit_termination() {
  return !_cm->in_conc_mark_from_roots() || has_aborted();
}

void G1ConcurrentPrefetch::clear_statistics_in_region(uint region_idx) {
  for (uint j = 0; j < _max_num_tasks; ++j) {
    _tasks[j]->clear_mark_stats_cache(region_idx);
  }
}



void G1ConcurrentPrefetch::reset_marking_for_restart() {
  // _global_mark_stack.set_empty();

  // Expand the marking stack, if we have to and if we can.
  // if (has_overflown()) {
  //   _global_mark_stack.expand();

  //   uint max_regions = _g1h->max_regions();
  //   for (uint i = 0; i < max_regions; i++) {
  //     _region_mark_stats[i].clear_during_overflow();
  //   }
  // }

  // clear_has_overflown();
  // _finger = _heap.start();

  for (uint i = 0; i < _max_num_tasks; ++i) {
    G1PFTaskQueue* queue = _task_queues->queue(i);
    queue->set_empty();
  }
}

void G1ConcurrentPrefetch::set_concurrency(uint active_tasks) {
  assert(active_tasks <= _max_num_tasks, "we should not have more");

  _num_active_tasks = active_tasks;
  // Need to update the three data structures below according to the
  // number of active threads for this phase.
  // _terminator = TaskTerminator((int) active_tasks, _task_queues);
  // _first_overflow_barrier_sync.set_n_workers((int) active_tasks);
  // _second_overflow_barrier_sync.set_n_workers((int) active_tasks);
}

void G1ConcurrentPrefetch::set_concurrency_and_phase(uint active_tasks, bool concurrent) {
  set_concurrency(active_tasks);

  _concurrent = concurrent;

  if (!concurrent) {
    ShouldNotReachHere();
    // At this point we should be in a STW phase, and completed marking.
    assert_at_safepoint_on_vm_thread();
    // assert(out_of_regions(),
    //        "only way to get here: _finger: " PTR_FORMAT ", _heap_end: " PTR_FORMAT,
    //        p2i(_finger), p2i(_heap.end()));
  }
}

void G1ConcurrentPrefetch::reset_at_marking_complete() {
  // We set the global marking state to some default values when we're
  // not doing marking.
  // reset_marking_for_restart();
  _num_active_tasks = 0;
}

G1ConcurrentPrefetch::~G1ConcurrentPrefetch() {
  // FREE_C_HEAP_ARRAY(HeapWord*, _top_at_rebuild_starts);
  FREE_C_HEAP_ARRAY(G1RegionMarkStats, _region_mark_stats);
  // The G1ConcurrentMark instance is never freed.
  ShouldNotReachHere();
}

void G1ConcurrentPrefetch::pre_initial_mark() {
  // Initialize marking structures. This has to be done in a STW phase.
  reset();

  // For each region note start of marking.
  // NoteStartOfMarkHRClosure startcl;
  // _g1h->heap_region_iterate(&startcl);

  // _root_regions.reset();
}

class G1PFConcurrentPrefetchingTask : public AbstractGangTask {
  G1ConcurrentMark*     _cm;
  G1ConcurrentPrefetch*     _pf;

public:
  void work(uint worker_id) {
    assert(Thread::current()->is_ConcurrentGC_thread(), "Not a concurrent GC thread");
    ResourceMark rm;

    double start_vtime = os::elapsedVTime();

    {
      SuspendibleThreadSetJoiner sts_join;

    //   assert(worker_id < _pf->active_tasks(), "invariant");

      G1PFTask* task = _pf->task(worker_id);
      task->record_start_time();
      if (!_cm->has_aborted()) {
        do {
          // G1TaskQueueEntry entry;

          size_t index = _pf->prefetch_queue_index();
          JavaThreadIteratorWithHandle jtiwh;
          JavaThread *t;
          PrefetchQueue* prefetch_queue;
          while(index) {
            t = jtiwh.next();
            if(t == NULL) {
              jtiwh.rewind();
              t = jtiwh.next();
            }
            index--;
          }
          bool get_queue = 0;
          while(_cm->in_conc_mark_from_roots() && !_cm->has_aborted()) {
            if(t == NULL) {
              jtiwh.rewind();
              t = jtiwh.next();
            }
            if(t == NULL) {
              ShouldNotReachHere();
            }
            prefetch_queue = &G1ThreadLocalData::prefetch_queue(t);
            if(jtiwh.list()->includes(t)&&prefetch_queue->set_in_processing()) {
              get_queue = 1;
              break;
            }
            t = jtiwh.next();
            if(t == NULL) {
              jtiwh.rewind();
              t = jtiwh.next();
            }
          }
          if(get_queue) {
            void* ptr;
            {
              MutexLockerEx z(prefetch_queue->locker(), Mutex::_no_safepoint_check_flag);
              bool ret = prefetch_queue->dequeue_no_lock(&ptr);
              while (ret && ptr != NULL) {
                if(!G1CollectedHeap::heap()->is_in_g1_reserved(ptr)) break;
                // bool success = task->make_prefetch_reference_black((oop)(HeapWord*)ptr);
                bool success = task->make_reference_grey((oop)(HeapWord*)ptr);

                if(success) {
                  // log_debug(prefetch)("Succesfully mark one in PFTask!");
                }
                ret = prefetch_queue->dequeue_no_lock(&ptr);
               }
             }
            prefetch_queue->release_processing();
            task->do_marking_step();
            _pf->do_yield_check();
          }

        } while (_cm->in_conc_mark_from_roots() && !_cm->has_aborted() && !task->has_aborted());
      }

      if ( _cm->has_aborted() || task->has_aborted() ){
        _pf->set_has_aborted();
      }

      task->record_end_time();
      log_debug(prefetch)("G1PFConcurrentPrefetchingTask duration %lf ms", task->_elapsed_time_ms);
      // guarantee(!_cm->concurrent(), "invariant");
    }

    double end_vtime = os::elapsedVTime();
    _pf->update_accum_task_vtime(worker_id, end_vtime - start_vtime);
  }

  G1PFConcurrentPrefetchingTask(G1ConcurrentMark* cm, G1ConcurrentPrefetch* pf) :
      AbstractGangTask("Concurrent Prefetch"), _cm(cm), _pf(pf) { }

  ~G1PFConcurrentPrefetchingTask() { }
};



void G1ConcurrentPrefetch::mark_from_stacks() {
  // _restart_for_overflow = false;

  _num_concurrent_workers = _max_concurrent_workers;

  uint active_workers = MAX2(1U, _num_concurrent_workers);

  // Setting active workers is not guaranteed since fewer
  // worker threads may currently exist and more may not be
  // available.
  active_workers = _concurrent_workers->update_active_workers(active_workers);
  log_info(gc, task)("Using %u workers of %u for marking", active_workers, _concurrent_workers->total_workers());

  // Parallel task terminator is set in "set_concurrency_and_phase()"
  set_concurrency_and_phase(active_workers, true /* concurrent */);

  G1PFConcurrentPrefetchingTask marking_task(_cm, this);
  _concurrent_workers->run_task(&marking_task);
  // print_stats();
}

// void G1ConcurrentPrefetch::verify_during_pause(G1HeapVerifier::G1VerifyType type, VerifyOption vo, const char* caller) {
//   G1HeapVerifier* verifier = _g1h->verifier();

//   verifier->verify_region_sets_optional();

//   if (VerifyDuringGC) {
//     GCTraceTime(Debug, gc, phases) debug(caller, _gc_timer_cm);

//     size_t const BufLen = 512;
//     char buffer[BufLen];

//     jio_snprintf(buffer, BufLen, "During GC (%s)", caller);
//     verifier->verify(type, vo, buffer);
//   }

//   verifier->check_bitmaps(caller);
// }





void G1ConcurrentPrefetch::flush_all_task_caches() {
  size_t hits = 0;
  size_t misses = 0;
  for (uint i = 0; i < _max_num_tasks; i++) {
    Pair<size_t, size_t> stats = _tasks[i]->flush_mark_stats_cache();
    hits += stats.first;
    misses += stats.second;
  }
  size_t sum = hits + misses;
  log_debug(gc, stats)("Mark stats cache hits " SIZE_FORMAT " misses " SIZE_FORMAT " ratio %1.3lf",
                       hits, misses, percent_of(hits, sum));
}




void G1PFTask::set_cm_oop_closure(G1PFOopClosure* cm_oop_closure) {
  if (cm_oop_closure == NULL) {
    assert(_cm_oop_closure != NULL, "invariant");
  } else {
    assert(_cm_oop_closure == NULL, "invariant");
  }
  _cm_oop_closure = cm_oop_closure;
}

void G1PFTask::reset(G1CMBitMap* next_mark_bitmap, G1CMBitMap* next_black_mark_bitmap) {
  guarantee(next_mark_bitmap != NULL, "invariant");
  _next_mark_bitmap              = next_mark_bitmap;
  _next_black_mark_bitmap              = next_black_mark_bitmap;
  // clear_region_fields();

  _calls                         = 0;
  _elapsed_time_ms               = 0.0;
  _termination_time_ms           = 0.0;
  _termination_start_time_ms     = 0.0;

  _mark_stats_cache.reset();
}

// bool G1CMTask::should_exit_termination() {
//   if (!regular_clock_call()) {
//     return true;
//   }

//   // This is called when we are in the termination protocol. We should
//   // quit if, for some reason, this task wants to abort or the global
//   // stack is not empty (this means that we can get work from it).
//   return !_cm->mark_stack_empty() || has_aborted();
// }


void G1PFTask::move_entries_to_global_stack() {
  // Local array where we'll store the entries that will be popped
  // from the local queue.
  G1TaskQueueEntry buffer[G1CMMarkStack::EntriesPerChunk];

  size_t n = 0;
  G1TaskQueueEntry task_entry;
  while (n < G1CMMarkStack::EntriesPerChunk && _task_queue->pop_local(task_entry)) {
    buffer[n] = task_entry;
    ++n;
  }
  if (n < G1CMMarkStack::EntriesPerChunk) {
    buffer[n] = G1TaskQueueEntry();
  }

  if (n > 0) {
    if (!_cm->mark_stack_push(buffer)) {
      // ShouldNotReachHere();
      set_has_aborted();
    }
  }
  // This operation was quite expensive, so decrease the limits.
  // decrease_limits();
}


void G1PFTask::drain_local_queue(bool partially) {
  if (has_aborted()) {
    return;
  }
  size_t max_num_objects = PrefetchNum;
  size_t max_size = PrefetchSize;
  
  G1TaskQueueEntry entry;
  // if(_words_scanned < max_size && _objs_scanned < max_num_objects) {
  //   bool ret = _task_queue->pop_global(entry);
  //   if(ret) scan_task_entry(entry);
  //   while (ret && _objs_scanned < max_num_objects && _words_scanned < max_size) {
  //     scan_task_entry(entry);
  //     ret = _task_queue->pop_global(entry);
  //   }
  // }
  while(_words_scanned < max_size && _objs_scanned < max_num_objects && !_cm->has_aborted() && !has_aborted()) {
    // bool ret = _task_queue->pop_global(entry);
    bool ret = _task_queue->pop_local(entry);

    if (ret) {
      size_t addr;
      if(entry.is_array_slice()){
        addr = (size_t)entry.slice();
      }else{
        addr = cast_from_oop<size_t>(entry.obj());
      }
      size_t mask_addr = addr & ((1ULL<<63)-1);
      size_t page_id = (mask_addr - SEMERU_START_ADDR)/4096;
      bool page_likely_local = _g1h->user_buf->page_stats[page_id] == 0;

      if(page_likely_local){
        _count_local_queue_page_local += 1;
      } else {
        _count_local_queue_page_remote += 1;
      }
    }

    if(ret) scan_task_entry(entry);
    else break;
  }
  if(_words_scanned>0)
    log_debug(prefetch)("_word_scanned: %lu, _objs_scanned: %lu", _words_scanned, _objs_scanned);
  if(!_cm->has_aborted() && !has_aborted())
    move_entries_to_global_stack();
  else{
    _task_queue->set_empty();
  }
}

void G1PFTask::clear_mark_stats_cache(uint region_idx) {
  _mark_stats_cache.reset(region_idx);
}

Pair<size_t, size_t> G1PFTask::flush_mark_stats_cache() {
  return _mark_stats_cache.evict_all();
}

// void G1CMTask::print_stats() {
//   log_debug(gc, stats)("Marking Stats, task = %u, calls = %u", _worker_id, _calls);
//   log_debug(gc, stats)("  Elapsed time = %1.2lfms, Termination time = %1.2lfms",
//                        _elapsed_time_ms, _termination_time_ms);
//   log_debug(gc, stats)("  Step Times (cum): num = %d, avg = %1.2lfms, sd = %1.2lfms max = %1.2lfms, total = %1.2lfms",
//                        _step_times_ms.num(),
//                        _step_times_ms.avg(),
//                        _step_times_ms.sd(),
//                        _step_times_ms.maximum(),
//                        _step_times_ms.sum());
//   size_t const hits = _mark_stats_cache.hits();
//   size_t const misses = _mark_stats_cache.misses();
//   log_debug(gc, stats)("  Mark Stats Cache: hits " SIZE_FORMAT " misses " SIZE_FORMAT " ratio %.3f",
//                        hits, misses, percent_of(hits, hits + misses));
// }

// bool G1ConcurrentMark::try_stealing(uint worker_id, G1TaskQueueEntry& task_entry) {
//   return _task_queues->steal(worker_id, task_entry);
// }

// /*****************************************************************************

//     The do_marking_step(time_target_ms, ...) method is the building
//     block of the parallel marking framework. It can be called in parallel
//     with other invocations of do_marking_step() on different tasks
//     (but only one per task, obviously) and concurrently with the
//     mutator threads, or during remark, hence it eliminates the need
//     for two versions of the code. When called during remark, it will
//     pick up from where the task left off during the concurrent marking
//     phase. Interestingly, tasks are also claimable during evacuation
//     pauses too, since do_marking_step() ensures that it aborts before
//     it needs to yield.

//     The data structures that it uses to do marking work are the
//     following:

//       (1) Marking Bitmap. If there are gray objects that appear only
//       on the bitmap (this happens either when dealing with an overflow
//       or when the initial marking phase has simply marked the roots
//       and didn't push them on the stack), then tasks claim heap
//       regions whose bitmap they then scan to find gray objects. A
//       global finger indicates where the end of the last claimed region
//       is. A local finger indicates how far into the region a task has
//       scanned. The two fingers are used to determine how to gray an
//       object (i.e. whether simply marking it is OK, as it will be
//       visited by a task in the future, or whether it needs to be also
//       pushed on a stack).

//       (2) Local Queue. The local queue of the task which is accessed
//       reasonably efficiently by the task. Other tasks can steal from
//       it when they run out of work. Throughout the marking phase, a
//       task attempts to keep its local queue short but not totally
//       empty, so that entries are available for stealing by other
//       tasks. Only when there is no more work, a task will totally
//       drain its local queue.

//       (3) Global Mark Stack. This handles local queue overflow. During
//       marking only sets of entries are moved between it and the local
//       queues, as access to it requires a mutex and more fine-grain
//       interaction with it which might cause contention. If it
//       overflows, then the marking phase should restart and iterate
//       over the bitmap to identify gray objects. Throughout the marking
//       phase, tasks attempt to keep the global mark stack at a small
//       length but not totally empty, so that entries are available for
//       popping by other tasks. Only when there is no more work, tasks
//       will totally drain the global mark stack.

//       (4) SATB Buffer Queue. This is where completed SATB buffers are
//       made available. Buffers are regularly removed from this queue
//       and scanned for roots, so that the queue doesn't get too
//       long. During remark, all completed buffers are processed, as
//       well as the filled in parts of any uncompleted buffers.

//     The do_marking_step() method tries to abort when the time target
//     has been reached. There are a few other cases when the
//     do_marking_step() method also aborts:

//       (1) When the marking phase has been aborted (after a Full GC).

//       (2) When a global overflow (on the global stack) has been
//       triggered. Before the task aborts, it will actually sync up with
//       the other tasks to ensure that all the marking data structures
//       (local queues, stacks, fingers etc.)  are re-initialized so that
//       when do_marking_step() completes, the marking phase can
//       immediately restart.

//       (3) When enough completed SATB buffers are available. The
//       do_marking_step() method only tries to drain SATB buffers right
//       at the beginning. So, if enough buffers are available, the
//       marking step aborts and the SATB buffers are processed at
//       the beginning of the next invocation.

//       (4) To yield. when we have to yield then we abort and yield
//       right at the end of do_marking_step(). This saves us from a lot
//       of hassle as, by yielding we might allow a Full GC. If this
//       happens then objects will be compacted underneath our feet, the
//       heap might shrink, etc. We save checking for this by just
//       aborting and doing the yield right at the end.

//     From the above it follows that the do_marking_step() method should
//     be called in a loop (or, otherwise, regularly) until it completes.

//     If a marking step completes without its has_aborted() flag being
//     true, it means it has completed the current marking phase (and
//     also all other marking tasks have done so and have all synced up).

//     A method called regular_clock_call() is invoked "regularly" (in
//     sub ms intervals) throughout marking. It is this clock method that
//     checks all the abort conditions which were mentioned above and
//     decides when the task should abort. A work-based scheme is used to
//     trigger this clock method: when the number of object words the
//     marking phase has scanned or the number of references the marking
//     phase has visited reach a given limit. Additional invocations to
//     the method clock have been planted in a few other strategic places
//     too. The initial reason for the clock method was to avoid calling
//     vtime too regularly, as it is quite expensive. So, once it was in
//     place, it was natural to piggy-back all the other conditions on it
//     too and not constantly check them throughout the code.

//     If do_termination is true then do_marking_step will enter its
//     termination protocol.

//     The value of is_serial must be true when do_marking_step is being
//     called serially (i.e. by the VMThread) and do_marking_step should
//     skip any synchronization in the termination and overflow code.
//     Examples include the serial remark code and the serial reference
//     processing closures.

//     The value of is_serial must be false when do_marking_step is
//     being called by any of the worker threads in a work gang.
//     Examples include the concurrent marking code (CMMarkingTask),
//     the MT remark code, and the MT reference processing closures.

//  *****************************************************************************/

void G1PFTask::do_marking_step() {
//   assert(time_target_ms >= 1.0, "minimum granularity is 1ms");

  _start_time_ms = os::elapsedVTime() * 1000.0;

  // If do_stealing is true then do_marking_step will attempt to
  // steal work from the other G1CMTasks. It only makes sense to
  // enable stealing when the termination protocol is enabled
  // and do_marking_step() is not being called serially.
  // bool do_stealing = do_termination && !is_serial;

  // double diff_prediction_ms = _g1h->g1_policy()->predictor().get_new_prediction(&_marking_step_diffs_ms);
  // _time_target_ms = time_target_ms - diff_prediction_ms;

  // set up the variables that are used in the work-based scheme to
  // call the regular clock method
  _words_scanned = 0;
  _objs_scanned = 0;
  _refs_reached  = 0;
  // recalculate_limits();

  // clear all flags
  clear_has_aborted();
  _has_timed_out = false;
  _draining_satb_buffers = false;

  ++_calls;

  // Set up the bitmap and oop closures. Anything that uses them is
  // eventually called from this method, so it is OK to allocate these
  // statically.
  G1PFOopClosure cm_oop_closure(_g1h, this);
  set_cm_oop_closure(&cm_oop_closure);
  // ...then partially drain the local queue and the global stack

  if (_cm->has_overflown()) {
    // This can happen if the mark stack overflows during a GC pause
    // and this task, after a yield point, restarts. We have to abort
    // as we need to get into the overflow protocol which happens
    // right at the end of this task.
    set_has_aborted();
  }

  drain_local_queue(true);
  
  // drain_global_stack(true);

  // do {
  //   if (!has_aborted() && _curr_region != NULL) {
  //     // This means that we're already holding on to a region.
  //     assert(_finger != NULL, "if region is not NULL, then the finger "
  //            "should not be NULL either");

  //     // We might have restarted this task after an evacuation pause
  //     // which might have evacuated the region we're holding on to
  //     // underneath our feet. Let's read its limit again to make sure
  //     // that we do not iterate over a region of the heap that
  //     // contains garbage (update_region_limit() will also move
  //     // _finger to the start of the region if it is found empty).
  //     update_region_limit();
  //     // We will start from _finger not from the start of the region,
  //     // as we might be restarting this task after aborting half-way
  //     // through scanning this region. In this case, _finger points to
  //     // the address where we last found a marked object. If this is a
  //     // fresh region, _finger points to start().
  //     MemRegion mr = MemRegion(_finger, _region_limit);

  //     assert(!_curr_region->is_humongous() || mr.start() == _curr_region->bottom(),
  //            "humongous regions should go around loop once only");

  //     // Some special cases:
  //     // If the memory region is empty, we can just give up the region.
  //     // If the current region is humongous then we only need to check
  //     // the bitmap for the bit associated with the start of the object,
  //     // scan the object if it's live, and give up the region.
  //     // Otherwise, let's iterate over the bitmap of the part of the region
  //     // that is left.
  //     // If the iteration is successful, give up the region.
  //     if (mr.is_empty()) {
  //       giveup_current_region();
  //       abort_marking_if_regular_check_fail();
  //     } else if (_curr_region->is_humongous() && mr.start() == _curr_region->bottom()) {
  //       if (_next_mark_bitmap->is_marked(mr.start())) {
  //         // The object is marked - apply the closure
  //         bitmap_closure.do_addr(mr.start());
  //       }
  //       // Even if this task aborted while scanning the humongous object
  //       // we can (and should) give up the current region.
  //       giveup_current_region();
  //       abort_marking_if_regular_check_fail();
  //     } else if (_next_mark_bitmap->iterate(&bitmap_closure, mr)) {
  //       giveup_current_region();
  //       abort_marking_if_regular_check_fail();
  //     } else {
  //       assert(has_aborted(), "currently the only way to do so");
  //       // The only way to abort the bitmap iteration is to return
  //       // false from the do_bit() method. However, inside the
  //       // do_bit() method we move the _finger to point to the
  //       // object currently being looked at. So, if we bail out, we
  //       // have definitely set _finger to something non-null.
  //       assert(_finger != NULL, "invariant");

  //       // Region iteration was actually aborted. So now _finger
  //       // points to the address of the object we last scanned. If we
  //       // leave it there, when we restart this task, we will rescan
  //       // the object. It is easy to avoid this. We move the finger by
  //       // enough to point to the next possible object header.
  //       assert(_finger < _region_limit, "invariant");
  //       HeapWord* const new_finger = _finger + ((oop)_finger)->size();
  //       // Check if bitmap iteration was aborted while scanning the last object
  //       if (new_finger >= _region_limit) {
  //         giveup_current_region();
  //       } else {
  //         move_finger_to(new_finger);
  //       }
  //     }
  //   }
  //   // At this point we have either completed iterating over the
  //   // region we were holding on to, or we have aborted.

  //   // We then partially drain the local queue and the global stack.
  //   // (Do we really need this?)
  //   drain_local_queue(true);
  //   drain_global_stack(true);

  //   // Read the note on the claim_region() method on why it might
  //   // return NULL with potentially more regions available for
  //   // claiming and why we have to check out_of_regions() to determine
  //   // whether we're done or not.
  //   while (!has_aborted() && _curr_region == NULL && !_cm->out_of_regions()) {
  //     // We are going to try to claim a new region. We should have
  //     // given up on the previous one.
  //     // Separated the asserts so that we know which one fires.
  //     assert(_curr_region  == NULL, "invariant");
  //     assert(_finger       == NULL, "invariant");
  //     assert(_region_limit == NULL, "invariant");
  //     HeapRegion* claimed_region = _cm->claim_region(_worker_id);
  //     if (claimed_region != NULL) {
  //       // Yes, we managed to claim one
  //       setup_for_region(claimed_region);
  //       assert(_curr_region == claimed_region, "invariant");
  //     }
  //     // It is important to call the regular clock here. It might take
  //     // a while to claim a region if, for example, we hit a large
  //     // block of empty regions. So we need to call the regular clock
  //     // method once round the loop to make sure it's called
  //     // frequently enough.
  //     abort_marking_if_regular_check_fail();
  //   }

  //   if (!has_aborted() && _curr_region == NULL) {
  //     assert(_cm->out_of_regions(),
  //            "at this point we should be out of regions");
  //   }
  // } while ( _curr_region != NULL && !has_aborted());

  // if (!has_aborted()) {
  //   // We cannot check whether the global stack is empty, since other
  //   // tasks might be pushing objects to it concurrently.
  //   assert(_cm->out_of_regions(),
  //          "at this point we should be out of regions");
  //   // Try to reduce the number of available SATB buffers so that
  //   // remark has less work to do.
  //   drain_satb_buffers();
  // }

  // // Since we've done everything else, we can now totally drain the
  // // local queue and global stack.
  // drain_local_queue(false);
  // drain_global_stack(false);

  // // Attempt at work stealing from other task's queues.
  // if (do_stealing && !has_aborted()) {
  //   // We have not aborted. This means that we have finished all that
  //   // we could. Let's try to do some stealing...

  //   // We cannot check whether the global stack is empty, since other
  //   // tasks might be pushing objects to it concurrently.
  //   assert(_cm->out_of_regions() && _task_queue->size() == 0,
  //          "only way to reach here");
  //   while (!has_aborted()) {
  //     G1TaskQueueEntry entry;
  //     if (_cm->try_stealing(_worker_id, entry)) {
  //       scan_task_entry(entry);

  //       // And since we're towards the end, let's totally drain the
  //       // local queue and global stack.
  //       drain_local_queue(false);
  //       drain_global_stack(false);
  //     } else {
  //       break;
  //     }
  //   }
  // }

  // // We still haven't aborted. Now, let's try to get into the
  // // termination protocol.
  // if (do_termination && !has_aborted()) {
  //   // We cannot check whether the global stack is empty, since other
  //   // tasks might be concurrently pushing objects on it.
  //   // Separated the asserts so that we know which one fires.
  //   assert(_cm->out_of_regions(), "only way to reach here");
  //   assert(_task_queue->size() == 0, "only way to reach here");
  //   _termination_start_time_ms = os::elapsedVTime() * 1000.0;

  //   // The G1CMTask class also extends the TerminatorTerminator class,
  //   // hence its should_exit_termination() method will also decide
  //   // whether to exit the termination protocol or not.
  //   bool finished = (is_serial ||
  //                    _cm->terminator()->offer_termination(this));
  //   double termination_end_time_ms = os::elapsedVTime() * 1000.0;
  //   _termination_time_ms +=
  //     termination_end_time_ms - _termination_start_time_ms;

  //   if (finished) {
  //     // We're all done.

  //     // We can now guarantee that the global stack is empty, since
  //     // all other tasks have finished. We separated the guarantees so
  //     // that, if a condition is false, we can immediately find out
  //     // which one.
  //     guarantee(_cm->out_of_regions(), "only way to reach here");
  //     guarantee(_cm->mark_stack_empty(), "only way to reach here");
  //     guarantee(_task_queue->size() == 0, "only way to reach here");
  //     guarantee(!_cm->has_overflown(), "only way to reach here");
  //     guarantee(!has_aborted(), "should never happen if termination has completed");
  //   } else {
  //     // Apparently there's more work to do. Let's abort this task. It
  //     // will restart it and we can hopefully find more things to do.
  //     set_has_aborted();
  //   }
  // }

  // // Mainly for debugging purposes to make sure that a pointer to the
  // // closure which was statically allocated in this frame doesn't
  // // escape it by accident.
  set_cm_oop_closure(NULL);
  // double end_time_ms = os::elapsedVTime() * 1000.0;
  // double elapsed_time_ms = end_time_ms - _start_time_ms;
  // // Update the step history.
  // _step_times_ms.add(elapsed_time_ms);

  // if (has_aborted()) {
  //   // The task was aborted for some reason.
  //   if (_has_timed_out) {
  //     double diff_ms = elapsed_time_ms - _time_target_ms;
  //     // Keep statistics of how well we did with respect to hitting
  //     // our target only if we actually timed out (if we aborted for
  //     // other reasons, then the results might get skewed).
  //     _marking_step_diffs_ms.add(diff_ms);
  //   }

  //   if (_cm->has_overflown()) {
  //     // This is the interesting one. We aborted because a global
  //     // overflow was raised. This means we have to restart the
  //     // marking phase and start iterating over regions. However, in
  //     // order to do this we have to make sure that all tasks stop
  //     // what they are doing and re-initialize in a safe manner. We
  //     // will achieve this with the use of two barrier sync points.

  //     if (!is_serial) {
  //       // We only need to enter the sync barrier if being called
  //       // from a parallel context
  //       _cm->enter_first_sync_barrier(_worker_id);

  //       // When we exit this sync barrier we know that all tasks have
  //       // stopped doing marking work. So, it's now safe to
  //       // re-initialize our data structures.
  //     }

  //     clear_region_fields();
  //     flush_mark_stats_cache();

  //     if (!is_serial) {
  //       // If we're executing the concurrent phase of marking, reset the marking
  //       // state; otherwise the marking state is reset after reference processing,
  //       // during the remark pause.
  //       // If we reset here as a result of an overflow during the remark we will
  //       // see assertion failures from any subsequent set_concurrency_and_phase()
  //       // calls.
  //       if (_cm->concurrent() && _worker_id == 0) {
  //         // Worker 0 is responsible for clearing the global data structures because
  //         // of an overflow. During STW we should not clear the overflow flag (in
  //         // G1ConcurrentMark::reset_marking_state()) since we rely on it being true when we exit
  //         // method to abort the pause and restart concurrent marking.
  //         _cm->reset_marking_for_restart();

  //         log_info(gc, marking)("Concurrent Mark reset for overflow");
  //       }

  //       // ...and enter the second barrier.
  //       _cm->enter_second_sync_barrier(_worker_id);
  //     }
  //     // At this point, if we're during the concurrent phase of
  //     // marking, everything has been re-initialized and we're
  //     // ready to restart.
  //   }
  // }
}

G1PFTask::G1PFTask(uint worker_id,
                   G1ConcurrentMark* cm,
                   G1ConcurrentPrefetch* pf,
                   G1PFTaskQueue* task_queue,
                   G1RegionMarkStats* mark_stats,
                   uint max_regions) :
  _objArray_processor(this),
  _worker_id(worker_id),
  _g1h(G1CollectedHeap::heap()),
  _cm(cm),
  _pf(pf),
  _next_mark_bitmap(NULL),
  _task_queue(task_queue),
  _mark_stats_cache(mark_stats, max_regions, RegionMarkStatsCacheSize),
  _calls(0),
  _time_target_ms(0.0),
  _start_time_ms(0.0),
  _cm_oop_closure(NULL),
  _curr_region(NULL),
  _finger(NULL),
  _region_limit(NULL),
  _words_scanned(0),
  _objs_scanned(0),
  _words_scanned_limit(0),
  _real_words_scanned_limit(0),
  _refs_reached(0),
  _refs_reached_limit(0),
  _real_refs_reached_limit(0),
  _has_aborted(false),
  _has_timed_out(false),
  _draining_satb_buffers(false),
  _step_times_ms(),
  _elapsed_time_ms(0.0),
  _termination_time_ms(0.0),
  _termination_start_time_ms(0.0),
  _marking_step_diffs_ms(),
  _count_local_queue_page_local(0),
  _count_local_queue_page_remote(0),
  _count_prefetch_white(0),
  _count_prefetch_grey(0),
  _count_prefetch_black(0),
  _count_steal(0)
{
  guarantee(task_queue != NULL, "invariant");

  _marking_step_diffs_ms.add(0.5);
}


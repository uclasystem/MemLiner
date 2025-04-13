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

#ifndef SHARE_GC_SHARED_PREFETCHQUEUE_HPP
#define SHARE_GC_SHARED_PREFETCHQUEUE_HPP

#include "gc/shared/ptrQueue.hpp"
#include "memory/allocation.hpp"

class JavaThread;
class PrefetchQueueSet;

// Base class for processing the contents of a SATB buffer.
class PrefetchBufferClosure : public StackObj {
protected:
  ~PrefetchBufferClosure() { }

public:
  // Process the SATB entries in the designated buffer range.
  virtual void do_buffer(void** buffer, size_t size) = 0;
};

// A PtrQueue whose elements are (possibly stale) pointers to object heads.
class PrefetchQueue: public PtrQueue {
  friend class PrefetchQueueSet;

private:

  Mutex _m;

  bool _in_dequeue;

  bool _in_processing;
  // Filter out unwanted entries from the buffer.
  inline void filter();

  // Removes entries from the buffer that are no longer needed.
  template<typename Filter>
  inline void apply_filter(Filter filter_out);

  

public:
  PrefetchQueue(PrefetchQueueSet* qset, bool permanent = false);
  ~PrefetchQueue() {
    if(_buf!=NULL){
      BufferNode* node = BufferNode::make_node_from_buffer(_buf, index());
      qset()->deallocate_buffer(node);
      _buf=NULL;
    }
  }

  Mutex* locker(){
    return &_m;
  }

  bool set_in_processing() {
    if(_in_processing == true) return false;
    if(Atomic::cmpxchg(true, &_in_processing, false ) == false) return true;
    return false;
  }

  void release_processing() {
    _in_processing = false;
  }

  // Process queue entries and free resources.
  void flush();

  // Apply cl to the active part of the buffer.
  // Prerequisite: Must be at a safepoint.
  void apply_closure_and_empty(PrefetchBufferClosure* cl);

  // Overrides PtrQueue::should_enqueue_buffer(). See the method's
  // definition for more information.
  virtual bool should_enqueue_buffer();

  // Compiler support.
  static ByteSize byte_offset_of_index() {
    return PtrQueue::byte_offset_of_index<PrefetchQueue>();
  }
  using PtrQueue::byte_width_of_index;

  static ByteSize byte_offset_of_buf() {
    return PtrQueue::byte_offset_of_buf<PrefetchQueue>();
  }
  using PtrQueue::byte_width_of_buf;

  static ByteSize byte_offset_of_active() {
    return PtrQueue::byte_offset_of_active<PrefetchQueue>();
  }
  using PtrQueue::byte_width_of_active;




  // Haoran: newly written fns
  void enqueue(volatile void* ptr) {
    enqueue((void*)(ptr));
  }

  // Enqueues the given "obj".
  void enqueue(void* ptr) {
    if (!_active) return;
    if (!Universe::heap()->is_in_reserved(ptr)) return;
    enqueue_known_active(ptr);
  }

  void enqueue_known_active(void* ptr)  {
    while (_index == 0) {
      handle_zero_index();
    }
    assert(_buf != NULL, "postcondition");
    assert(index() > 0, "postcondition");
    assert(index() <= capacity(), "invariant");
    _buf[index() - 1] = ptr;
    OrderAccess::storestore();
    _index -= _element_size;
  }

  size_t prefetch_queue_threshold() {
    return PrefetchQueueThreshold;
  }

  void handle_zero_index() {
    MutexLockerEx z(&_m, Mutex::_no_safepoint_check_flag);

    // while(_in_dequeue == true) {
    //   continue;
    // }
    // if(Atomic::cmpxchg(true, &_in_dequeue, false ) == true) {
    //   // _in_dequeue = false;
    //   return;
    // }
    // _in_dequeue = true;
    assert(index() == 0, "precondition");
    // This thread records the full buffer and allocates a new one (while
    // holding the lock if there is one).
    if (_buf != NULL) {
      // Two-fingered compaction toward the end.
      size_t remaining_objs = MIN2(prefetch_queue_threshold(), _tail-index());
      void** src = &_buf[index()];
      void** dst = &_buf[index() + remaining_objs - 1];
      void** end = &_buf[capacity() - 1];
      // assert(src <= dst, "invariant");
      for ( ; src <= dst; dst --, end --) {
        *end = *dst;
      }
      // dst points to the lowest retained entry, or the end of the buffer
      // if all the entries were filtered out.
      set_index(capacity() - remaining_objs);
      _tail = capacity();
    }
    else {
      // Set capacity in case this is the first allocation.
      set_capacity(qset()->buffer_size());
      // Allocate a new buffer.
      // _buf = NEW_C_HEAP_ARRAY(void*, qset()->buffer_size(), mtGC)
      _buf = qset()->allocate_buffer();
      reset();
    }
    // _in_dequeue = false;
  }

  bool dequeue(void** ptrptr) { 

    // while(_in_dequeue == true) {
    //   continue;
    // }
    // if(Atomic::cmpxchg(true, &_in_dequeue, false ) == true) {
    //   *ptrptr = NULL;
    //   return false;
    // }
    // if(_in_dequeue == true) {
    //     *ptrptr = NULL;
    //     return false;
    // }
    MutexLockerEx z(&_m, Mutex::_no_safepoint_check_flag);
    size_t current_index = index();
    size_t current_tail = _tail; 
    if(current_tail - current_index <= PrefetchDelay) {
      *ptrptr = NULL;

      // _in_dequeue = false;
      return false;
    }
    _tail -= 1;
    *ptrptr = _buf[current_tail - 1];


    // _in_dequeue = false;
    return true;
    
  }

  bool dequeue_no_lock(void** ptrptr) { 

    // while(_in_dequeue == true) {
    //   continue;
    // }
    // if(Atomic::cmpxchg(true, &_in_dequeue, false ) == true) {
    //   *ptrptr = NULL;
    //   return false;
    // }
    // if(_in_dequeue == true) {
    //     *ptrptr = NULL;
    //     return false;
    // }
    size_t current_index = index();
    size_t current_tail = _tail; 
    if(current_tail - current_index <= PrefetchDelay) {
      *ptrptr = NULL;

      // _in_dequeue = false;
      return false;
    }
    _tail -= 1;
    *ptrptr = _buf[current_tail - 1];


    // _in_dequeue = false;
    return true;
    
  }
};

class PrefetchQueueSet: public PtrQueueSet {
  PrefetchQueue _shared_prefetch_queue;
  size_t _buffer_enqueue_threshold;


protected:
  PrefetchQueueSet();
  ~PrefetchQueueSet() {}

  template<typename Filter>
  void apply_filter(Filter filter, PrefetchQueue* queue) {
    queue->apply_filter(filter);
  }

  void initialize(Monitor* cbl_mon,
                  BufferNode::Allocator* allocator/*,
                  size_t process_completed_buffers_threshold,
                  uint buffer_enqueue_threshold_percentage,
                  Mutex* lock*/);

public:
  virtual PrefetchQueue& prefetch_queue_for_thread(JavaThread* const t) const = 0;

  // Apply "set_active(active)" to all SATB queues in the set. It should be
  // called only with the world stopped. The method will assert that the
  // SATB queues of all threads it visits, as well as the SATB queue
  // set itself, has an active value same as expected_active.
  void set_active_all_threads(bool active, bool expected_active);

  size_t buffer_enqueue_threshold() const { return _buffer_enqueue_threshold; }
  virtual void filter(PrefetchQueue* queue) = 0;

  // Filter all the currently-active SATB buffers.
  void filter_thread_buffers();

  // If there exists some completed buffer, pop and process it, and
  // return true.  Otherwise return false.  Processing a buffer
  // consists of applying the closure to the active range of the
  // buffer; the leading entries may be excluded due to filtering.
  bool apply_closure_to_completed_buffer(PrefetchBufferClosure* cl);



  PrefetchQueue* shared_prefetch_queue() { return &_shared_prefetch_queue; }

  // If a marking is being abandoned, reset any unprocessed log buffers.
  void abandon_partial_marking();
};

inline void PrefetchQueue::filter() {
  static_cast<PrefetchQueueSet*>(qset())->filter(this);
}

// Removes entries from the buffer that are no longer needed, as
// determined by filter. If e is a void* entry in the buffer,
// filter_out(e) must be a valid expression whose value is convertible
// to bool. Entries are removed (filtered out) if the result is true,
// retained if false.
template<typename Filter>
inline void PrefetchQueue::apply_filter(Filter filter_out) {
  void** buf = this->_buf;

  if (buf == NULL) {
    // nothing to do
    return;
  }

  // Two-fingered compaction toward the end.
  void** src = &buf[this->index()];
  void** dst = &buf[this->capacity()];
  assert(src <= dst, "invariant");
  for ( ; src < dst; ++src) {
    // Search low to high for an entry to keep.
    void* entry = *src;
    if (!filter_out(entry)) {
      // Found keeper.  Search high to low for an entry to discard.
      while (src < --dst) {
        if (filter_out(*dst)) {
          *dst = entry;         // Replace discard with keeper.
          break;
        }
      }
      // If discard search failed (src == dst), the outer loop will also end.
    }
  }
  // dst points to the lowest retained entry, or the end of the buffer
  // if all the entries were filtered out.
  this->set_index(dst - buf);
}

#endif // SHARE_GC_SHARED_SATBMARKQUEUE_HPP

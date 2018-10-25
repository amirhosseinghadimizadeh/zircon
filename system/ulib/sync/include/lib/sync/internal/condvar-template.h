// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYNC_INTERNAL_CONDVAR_TEMPLATE_H_
#define LIB_SYNC_INTERNAL_CONDVAR_TEMPLATE_H_

#include <zircon/syscalls.h>
#include <lib/sync/mtx.h>

namespace condvar_impl_internal {

// A template implementation of a condition variable.
// The algorithm is borrowed from MUSL.
//
// The 'Condvar' struct must contain the following fields:
//      int lock;
//      void* head;
//      void* tail;
//
// The following struct template must be specialized for the mutex type 'Mutex'
// in order to instantiate the template:
template <typename Mutex>
struct MutexOps {
    // Return a pointer to the futex that backs the |mutex|
    static zx_futex_t* get_futex(Mutex* mutex);

    // Lock the |mutex|. If an error occurs while locking the mutex,
    // ZX_ERR_BAD_STATE must be returned. An implementation-defined
    // error code can be returned via |mutex_lock_err| if it's not null.
    static zx_status_t lock(Mutex* mutex, int* mutex_lock_err);

    // Similar to lock(), but also update the waiter information in the mutex.
    // If the mutex implements waiter counting, then the count must be adjusted
    // by |waiters_delta|. Otherwise, the mutex must be marked as potentially
    // having waiters.
    static zx_status_t lock_with_waiters(
        Mutex* mutex, int waiters_delta, int* mutex_lock_err);

    // Unlock the mutex
    static void unlock(Mutex* mutex);
};

// Note that this library is used by libc, and as such needs to use
// '_zx_' function names for syscalls and not the regular 'zx_' names.

enum {
    UNLOCKED = 0,
    LOCKED_NO_WAITERS = 1,
    LOCKED_MAYBE_WAITERS = 2,
};

static inline void spin() {
#if defined(__x86_64__)
    __asm__ __volatile__("pause"
                         :
                         :
                         : "memory");
#elif defined(__aarch64__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
#error Please define spin() for your architecture
#endif
}

static inline bool cas(int* ptr, int* expected, int desired) {
    return __atomic_compare_exchange_n(ptr, expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void wait(int* futex, int current_value) {
    int spins = 100;
    while (spins--) {
        if (__atomic_load_n(futex, __ATOMIC_SEQ_CST) == current_value) {
            spin();
        } else {
            return;
        }
    }
    while (__atomic_load_n(futex, __ATOMIC_SEQ_CST) == current_value) {
        _zx_futex_wait(futex, current_value, ZX_TIME_INFINITE);
    }
}

// TODO(ZX-2882): consider replacing lock()/unlock() etc. with sync_completion
static inline void lock(int* l) {
    int old = UNLOCKED;
    if (!cas(l, &old, LOCKED_NO_WAITERS)) {
        old = LOCKED_NO_WAITERS;
        cas(l, &old, LOCKED_MAYBE_WAITERS);
        do {
            wait(l, LOCKED_MAYBE_WAITERS);
            old = UNLOCKED;
        } while (!cas(l, &old, LOCKED_MAYBE_WAITERS));
    }
}

static inline void unlock(int* l) {
    if (__atomic_exchange_n(l, UNLOCKED, __ATOMIC_SEQ_CST) == LOCKED_MAYBE_WAITERS) {
        _zx_futex_wake(l, 1);
    }
}

static inline void unlock_requeue(int* l, zx_futex_t* r) {
    __atomic_store_n(l, UNLOCKED, __ATOMIC_SEQ_CST);
    _zx_futex_requeue(l, /* wake count */ 0, /* l futex value */ UNLOCKED,
                      r, /* requeue count */ 1);
}

enum {
    WAITING,
    SIGNALED,
    LEAVING,
};

struct Waiter {
    Waiter* prev = nullptr;
    Waiter* next = nullptr;
    int state = WAITING;
    int barrier = LOCKED_MAYBE_WAITERS;
    int* notify = nullptr;
};

// Return value:
//      - ZX_OK if the condition variable was signaled;
//      - ZX_ERR_TIMED_OUT if deadline was reached;
//      - ZX_ERR_BAD_STATE if there was an error locking the mutex.
//        In this case, |mutex_lock_err|, if not null, will be populated with an error code
//        provided by the mutex implementation.
template <typename Condvar, typename Mutex>
static inline zx_status_t timedwait(Condvar* c, Mutex* mutex, zx_time_t deadline,
                                    int* mutex_lock_err)
    __TA_NO_THREAD_SAFETY_ANALYSIS {
    sync_mtx_lock(reinterpret_cast<sync_mtx_t*>(&c->lock));

    Waiter node;

    // Add our waiter node onto the condvar's list.  We add the node to the
    // head of the list, but this is logically the end of the queue.
    node.next = static_cast<Waiter*>(c->head);
    c->head = &node;
    if (!c->tail) {
        c->tail = &node;
    } else {
        node.next->prev = &node;
    }

    sync_mtx_unlock(reinterpret_cast<sync_mtx_t*>(&c->lock));

    MutexOps<Mutex>::unlock(mutex);

    // Wait to be signaled.  There are multiple ways this loop could exit:
    //  1) After being woken by signal().
    //  2) After being woken by a mutex unlock, after we were
    //     requeued from the condvar's futex to the mutex's futex (by
    //     timedwait() in another thread).
    //  3) After a timeout.
    // In the original Linux version of this algorithm, this could also exit
    // when interrupted by an asynchronous signal, but that does not apply on Zircon.
    zx_status_t status;
    while (true) {
        status = _zx_futex_wait(&node.barrier, LOCKED_MAYBE_WAITERS, deadline);
        if (status == ZX_ERR_TIMED_OUT) {
            break;
        }
        status = ZX_OK;
        if (__atomic_load_n(&node.barrier, __ATOMIC_SEQ_CST) != LOCKED_MAYBE_WAITERS) {
            break;
        }
    }

    int oldstate = WAITING;
    if (cas(&node.state, &oldstate, LEAVING)) {
        // The wait timed out.  So far, this thread was not signaled by
        // signal() -- this thread was able to move state.node out of the
        // WAITING state before any signal() call could do that.
        //
        // This thread must therefore remove the waiter node from the
        // list itself.

        // Access to cv object is valid because this waiter was not
        // yet signaled and a new signal/broadcast cannot return
        // after seeing a LEAVING waiter without getting notified
        // via the futex notify below.

        sync_mtx_lock(reinterpret_cast<sync_mtx_t*>(&c->lock));

        // Remove our waiter node from the list.
        if (c->head == &node) {
            c->head = node.next;
        } else if (node.prev) {
            node.prev->next = node.next;
        }

        if (c->tail == &node) {
            c->tail = node.prev;
        } else if (node.next) {
            node.next->prev = node.prev;
        }

        sync_mtx_unlock(reinterpret_cast<sync_mtx_t*>(&c->lock));

        // It is possible that signal() saw our waiter node after we set
        // node.state to LEAVING but before we removed the node from the
        // list.  If so, it will have set node.notify and will be waiting
        // on it, and we need to wake it up.
        //
        // This is rather complex.  An alternative would be to eliminate
        // the |node.state| field and always claim |lock| if we could have
        // got a timeout.  However, that presumably has higher overhead
        // (since it contends |lock| and involves more atomic ops).
        if (node.notify) {
            if (__atomic_fetch_add(node.notify, -1, __ATOMIC_SEQ_CST) == 1) {
                _zx_futex_wake(node.notify, 1);
            }
        }

        // We don't need lock_with_waiters() here: we haven't been signaled, and will
        // never be since we managed to claim the state as LEAVING. This means that
        // we could not have been woken up by unlock_requeue() + mutex unlock().
        if (MutexOps<Mutex>::lock(mutex, mutex_lock_err) != ZX_OK) {
            return ZX_ERR_BAD_STATE;
        }
        return ZX_ERR_TIMED_OUT;
    }

    // Lock barrier first to control wake order
    lock(&node.barrier);

    // By this point, our part of the waiter list cannot change further.
    // It has been unlinked from the condvar by signal().
    // Any timed out waiters would have removed themselves from the list
    // before signal() signaled the first node.barrier in our list.
    //
    // It is therefore safe now to read node.next and node.prev without
    // holding c->lock.

    // As an optimization, we only update waiter count at the beginning and
    // end of the signaled list.
    int waiters_delta = 0;
    if (!node.prev) {
        waiters_delta++;
    }
    if (!node.next) {
        waiters_delta--;
    }

    // We must leave the mutex in the "locked with waiters" state here
    // (or adjust its waiter count, depending on the implementation).
    // There are two reasons for that:
    //  1) If we do the unlock_requeue() below, a condvar waiter will be
    //     requeued to the mutex's futex.  We need to ensure that it will
    //     be signaled by mutex unlock() in future.
    //  2) If the current thread was woken via an unlock_requeue() +
    //     mutex unlock, there *might* be another thread waiting for
    //     the mutex after us in the queue.  We need to ensure that it
    //     will be signaled by zxr_mutex_unlock() in future.
    if (MutexOps<Mutex>::lock_with_waiters(mutex, waiters_delta, mutex_lock_err) != ZX_OK) {
        status = ZX_ERR_BAD_STATE;
    }

    if (node.prev) {
        // Unlock the barrier that's holding back the next waiter, and
        // requeue it to the mutex so that it will be woken when the
        // mutex is unlocked.
        unlock_requeue(&node.prev->barrier, MutexOps<Mutex>::get_futex(mutex));
    }

    return status;
}

// This will wake up to |n| threads that are waiting on the condvar,
// or all waiting threads if |n| is set to -1
template <typename Condvar>
static inline void signal(Condvar* c, int n) {
    Waiter* p;
    Waiter* first = nullptr;
    int ref = 0;
    int cur;

    sync_mtx_lock(reinterpret_cast<sync_mtx_t*>(&c->lock));
    for (p = static_cast<Waiter*>(c->tail); n && p; p = p->prev) {
        int oldstate = WAITING;
        if (!cas(&p->state, &oldstate, SIGNALED)) {
            // This waiter timed out, and it marked itself as in the
            // LEAVING state.  However, it hasn't yet claimed |lock|
            // (since we claimed the lock first) and so it hasn't yet
            // removed itself from the list.  We will wait for the waiter
            // to remove itself from the list and to notify us of that.
            __atomic_fetch_add(&ref, 1, __ATOMIC_SEQ_CST);
            p->notify = &ref;
        } else {
            n--;
            if (!first) {
                first = p;
            }
        }
    }
    // Split the list, leaving any remainder on the cv.
    if (p) {
        if (p->next) {
            p->next->prev = 0;
        }
        p->next = 0;
    } else {
        c->head = 0;
    }
    c->tail = p;
    sync_mtx_unlock(reinterpret_cast<sync_mtx_t*>(&c->lock));

    // Wait for any waiters in the LEAVING state to remove
    // themselves from the list before returning or allowing
    // signaled threads to proceed.
    while ((cur = __atomic_load_n(&ref, __ATOMIC_SEQ_CST))) {
        wait(&ref, cur);
    }

    // Allow first signaled waiter, if any, to proceed.
    if (first) {
        unlock(&first->barrier);
    }
}

} // namespace condvar_impl_internal

#endif // LIB_SYNC_INTERNAL_CONDVAR_TEMPLATE_H_

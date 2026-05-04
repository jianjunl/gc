
#include <pthread.h>
#include <errno.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h" 

typedef struct gc_global_mutex_t {
    pthread_mutex_t lock;        // Underlying non-recursive mutex
    pthread_t owner;             // Owning thread ID
    int lock_count;              // Recursive lock counter
} gc_global_mutext_t;

static gc_global_mutext_t gc_global_mutex = {
    .lock       = PTHREAD_MUTEX_INITIALIZER,
    .owner      = 0,
    .lock_count = 0
};

/*
 * Safety: In gc_global_lock, if the thread is already the owner, it is the only thread that could be
 * modifying lock_count. Therefore, incrementing it without holding a secondary lock is thread-safe.
 *
 * Atomicity: If the thread is not the owner, it hits pthread_mutex_lock. This blocks until the current
 *  owner (if any) sets lock_count to 0 and releases the mutex.
 *
 * Portability: Using pthread_equal is safer than == because on some platforms (like macOS or certain BSDs),
 *  pthread_t can be a pointer or a structure rather than a long.
 *
 * Zero-Initialization: Since we have explicitly initialized .owner = 0 and .lock_count = 0, the first call to
 * gc_global_lock will correctly fail the pthread_equal check and proceed to the initial pthread_mutex_lock.
*/
 
// Lock recursive mutex
int gc_global_lock() {
    pthread_t self = pthread_self();

    // Check if the current thread is already the owner
    // We check lock_count > 0 first to ensure the owner field is valid
    if (gc_global_mutex.lock_count > 0 && pthread_equal(gc_global_mutex.owner, self)) {
        gc_global_mutex.lock_count++;
        return 0;
    }

    // Not the owner (or first lock attempt), acquire the underlying mutex
    if (pthread_mutex_lock(&gc_global_mutex.lock) != 0) {
        return -1;
    }

    // Successfully locked for the first time
    gc_global_mutex.owner = self;
    gc_global_mutex.lock_count = 1;
    
    return 0;
}

// Unlock recursive mutex
int gc_global_unlock() {
    pthread_t self = pthread_self();

    // Safety check: ensure the caller actually owns the lock
    if (gc_global_mutex.lock_count <= 0 || !pthread_equal(gc_global_mutex.owner, self)) {
        return -1; 
    }

    // Decrement the recursion counter
    gc_global_mutex.lock_count--;

    // If the counter reaches zero, we release the physical lock
    if (gc_global_mutex.lock_count == 0) {
        // Optional: Reset owner to 0 for hygiene, though not strictly required
        // as the next owner will overwrite it.
        return pthread_mutex_unlock(&gc_global_mutex.lock);
    }

    return 0;
} 

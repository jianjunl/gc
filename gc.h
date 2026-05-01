/*
 * gc.h - Conservative Garbage Collector for C (Multi-threaded, setjmp/longjmp safe)
 *
 * This library provides a Boehm-style conservative garbage collector that
 * can be used as a drop-in replacement for malloc/calloc in C programs.
 * It supports multi-threading, precise root registration, and safe non-local
 * jumps via gc_setjmp/gc_longjmp.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 */

#ifndef GC_H
#define GC_H

#include <stddef.h>   /* for size_t */
#include <setjmp.h>   /* for jmp_buf / sigjmp_buf */
#include <signal.h>   /* for sigjmp_buf */
#include <pthread.h>  /* for pthread_t, pthread_attr_t */
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration Macros (define before including this header)
 * ------------------------------------------------------------------------- */
#ifndef GC_OPT_MULTITHREAD
#define GC_OPT_MULTITHREAD 0 /* set to 0 to disable multi-threading */
#endif

#ifndef GC_OPT_INCREMENTAL
#define GC_OPT_INCREMENTAL 0 /* set to 1 to enable incremental marking */
#endif

#ifndef GC_OPT_FINALIZING
#define GC_OPT_FINALIZING  1 /* set to 0 to disable finalizers & weak refs */
#endif

#ifndef GC_OPT_FREELIST
#define GC_OPT_FREELIST    0
#endif

#define GC_MASK(x, n) ((x) << (n))

#define GC_OPT (GC_MASK(GC_OPT_MULTITHREAD, 0)|\
                GC_MASK(GC_OPT_FREELIST, 1)|\
                GC_MASK(GC_OPT_INCREMENTAL, 2)|\
                GC_MASK(GC_OPT_FINALIZING & GC_OPT_MULTITHREAD, 3)) 

#define GC_MULTITHREAD (GC_OPT & 0x01) >> 0 
#define GC_FREELIST    (GC_OPT & 0x02) >> 1
#define GC_INCREMENTAL (GC_OPT & 0x04) >> 2
#define GC_FINALIZING  (GC_OPT & 0x08) >> 3

/* ============================================================================
 * Initialization and Statistics
 * ============================================================================ */

/*
 * gc_init - Initialize the garbage collector.
 *
 * This function must be called before any other GC functions if you want to
 * explicitly control initialization. However, gc_malloc() and gc_calloc()
 * will automatically call it on first use, so manual invocation is optional.
 *
 * It sets up signal handlers for stop-the-world, scans /proc/self/maps to
 * locate the main thread stack, and registers the main thread.
 */
void gc_init(void);

/*
 * gc_dump_stats - Print internal GC statistics to stderr.
 *
 * Outputs the total allocated bytes, number of live blocks, and the current
 * collection threshold. Useful for debugging and tuning.
 */
void gc_dump_stats(void);

/* ============================================================================
 * Memory Allocation
 * ============================================================================ */

/*
 * gc_malloc - Allocate a block of uninitialized memory.
 *
 * @size: number of bytes to allocate.
 * @return: pointer to allocated memory, or NULL on failure.
 *
 * The allocated memory is obtained from the operating system via mmap.
 * It is NOT initialized. The returned pointer is tracked by the collector
 * and will be automatically freed when no longer reachable.
 *
 * This function is thread-safe and may trigger a garbage collection cycle
 * if the total allocation exceeds the internal threshold.
 */
void* gc_malloc(size_t size);
char* gc_strdup(const char *src);
void **gc_root_malloc(size_t size);

/*
 * gc_calloc - Allocate and zero-initialize an array.
 *
 * @nmemb: number of elements.
 * @size:  size of each element.
 * @return: pointer to allocated and zeroed memory, or NULL on failure.
 *
 * Equivalent to gc_malloc(nmemb * size) followed by memset(..., 0).
 * Checks for multiplication overflow and returns NULL if detected.
 */
void* gc_calloc(size_t nmemb, size_t size);

/*
 * gc_free - Explicitly free a GC-managed object.
 *
 * @ptr: pointer returned by gc_malloc/gc_calloc.
 *
 * In a conservative collector, explicit freeing is generally unsafe
 * because other references may still exist. This function is provided
 * for compatibility and debugging; it currently does nothing.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
void gc_free(void *ptr);
#pragma GCC diagnostic pop

/*
 * gc_collect - Force a full garbage collection cycle.
 *
 * Performs a stop-the-world mark-and-sweep collection. All threads are
 * suspended (except the caller), roots are scanned (data segment, stacks,
 * registers, and registered precise roots), unreachable blocks are freed.
 *
 * This function is thread-safe and may be called at any time.
 */
void gc_collect(void);

/* ============================================================================
 * Precise Root Registration
 * ============================================================================ */

/*
 * gc_add_root - Register a pointer variable as a GC root.
 *
 * @ptr_addr: address of a pointer variable (e.g., &my_ptr).
 *
 * The pointer stored at *ptr_addr will be treated as a root during
 * collection. The object it points to, and everything reachable from it,
 * will be retained. This is essential for objects not discoverable by
 * conservative stack scanning (e.g., global variables in shared libraries,
 * or to overcome false negatives).
 *
 * The caller is responsible for calling gc_remove_root() when the pointer
 * variable is no longer needed.
 */
void gc_add_root(void *ptr_addr);

/*
 * gc_remove_root - Remove a previously registered root.
 *
 * @ptr_addr: the same address passed to gc_add_root().
 */
void gc_remove_root(void *ptr_addr);

/*
 * GC_ROOT - Convenience macro for stack-allocated precise roots.
 *
 * Usage:
 *   GC_ROOT(node_t, my_list) = gc_malloc(sizeof(node_t));
 *
 * This declares a pointer variable named 'my_list' of type 'node_t*',
 * automatically registers it as a root, and arranges for it to be
 * unregistered when the variable goes out of scope (using GCC's cleanup
 * attribute). Requires GCC or Clang.
 */
#define GC_ROOT(type, name) \
    type * name __attribute__((cleanup(gc_root_cleanup))) = NULL; \
    gc_add_root(&name)

/* Internal helper for GC_ROOT; do not call directly. */
void gc_root_cleanup(void **ptr_addr);

#if GC_FINALIZING == 1

/* ============================================================================
 * Weak References and Finalizers
 * ============================================================================ */

/*
 * gc_weak_ref_t - Opaque handle for a weak reference.
 *
 * Weak references allow the user to track an object without preventing its
 * collection. When the object is garbage collected, the weak reference is
 * automatically cleared (set to NULL) and an optional notification callback
 * is invoked.
 */
typedef struct gc_weak_ref gc_weak_ref_t;

/*
 * gc_make_weak - Create a weak reference to a GC-managed object.
 *
 * @obj: pointer to the object (must have been allocated with gc_malloc).
 * @on_clear: optional callback invoked when the object is collected.
 *            It receives the original pointer (now invalid) and user data.
 * @user_data: data passed to on_clear.
 *
 * Returns a handle that can be used to retrieve the object as long as it
 * remains alive. Once the object is collected, gc_weak_get() returns NULL.
 *
 * The returned handle must be destroyed with gc_weak_free() when no longer
 * needed.
 */
gc_weak_ref_t* gc_make_weak(void *obj, void (*on_clear)(void*, void*), void *user_data);

/*
 * gc_weak_get - Retrieve the object from a weak reference.
 *
 * @ref: weak reference handle.
 * Returns the original object pointer if it is still alive, otherwise NULL.
 */
void* gc_weak_get(gc_weak_ref_t *ref);

/*
 * gc_weak_free - Destroy a weak reference handle.
 *
 * @ref: weak reference handle. Does nothing if ref is NULL.
 */
void gc_weak_free(gc_weak_ref_t *ref);

/*
 * gc_register_finalizer - Register a function to be called when an object
 * is garbage collected.
 *
 * @obj: pointer to the GC-managed object.
 * @finalizer: function called with @obj and @user_data just before the
 *             object's memory is freed.
 * @user_data: data passed to finalizer.
 *
 * Finalizers are called during the sweep phase, after marking but before
 * the memory is unmapped. They should not access other GC-managed objects
 * unless those are known to be still reachable (e.g., via a precise root).
 *
 * Finalizers are executed in an arbitrary order and may be invoked from
 * any thread. Keep them short and async-signal-safe.
 */
void gc_register_finalizer(void *obj, void (*finalizer)(void*, void*), void *user_data);

#endif /* GC_FINALIZING */

/* ============================================================================
 * Exception handling (try / throw / catch)
 * ============================================================================ */

typedef struct gc_exception {
    sigjmp_buf          buf;
    int                 code;
    struct gc_exception *prev;   /* for nested TRY blocks */
} gc_exception_t;

extern _Thread_local gc_exception_t *gc_current_exception;

/*
 * TRY – start a guarded block.
 * CATCH – immediately follows the TRY block and is executed when an
 *         exception is thrown inside the TRY block.
 * THROW(e) – throw an exception with code e.
 *
 * Example:
 *   TRY {
 *       // guarded code
 *       THROW(42);
 *   }
 *   CATCH {
 *       printf("caught %d\n", gc_current_exception->code);
 *   }
 */
#define TRY \
    do { \
        gc_exception_t gc_exc; \
        gc_exc.prev = gc_current_exception; \
        gc_current_exception = &gc_exc; \
        gc_exc.code = 0; \
        if (sigsetjmp(gc_exc.buf, 1) == 0)

#define CATCH \
        else

#define END_TRY \
        gc_current_exception = gc_exc.prev; \
    } while (0)

#define THROW(code) gc_throw(code)
#define THROW_THROWN(void) gc_throw_thrown()
#define THROWN gc_current_exception->code

/* Prototype */
void gc_throw(int code);
void gc_throw_thrown(void);

/* ============================================================================
 * Thread Support
 * ============================================================================ */

#if GC_MULTITHREAD == 0

/* In single-threaded mode, map GC thread functions directly to pthread. */
#define gc_pthread_create(thread, attr, start, arg) pthread_create(thread, attr, start, arg)
#define gc_pthread_join(thread, retval) pthread_join(thread, retval)
#define gc_pthread_exit(retval) pthread_exit(retval)

#define gc_register_thread_stack(bottom) ((void)0)
#define gc_unregister_thread()           ((void)0)

#else /* GC_MULTITHREAD : Multi-threaded mode with full tracking */

/*
 * gc_pthread_create - Create a new thread tracked by the garbage collector.
 *
 * @thread:       pointer to pthread_t (same as pthread_create).
 * @attr:         thread attributes (may be NULL).
 * @start_routine: function to execute in the new thread.
 * @arg:          argument to pass to start_routine.
 * @return:       0 on success, error code on failure.
 *
 * This function wraps pthread_create. It ensures the new thread is registered
 * with the collector so that its stack and registers are scanned during
 * garbage collections. The thread's stack boundaries are automatically
 * discovered.
 *
 * Use this instead of raw pthread_create for threads that will use GC-managed
 * memory.
 */
int gc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine)(void*), void *arg);

/*
 * gc_pthread_join - Wait for a GC-tracked thread to terminate.
 *
 * @thread: thread to join.
 * @retval:  if non-NULL, receives the thread's return value.
 * @return:  0 on success, error code on failure.
 *
 * Equivalent to pthread_join. No special GC handling is needed.
 */
int gc_pthread_join(pthread_t thread, void **retval);

/*
 * gc_pthread_exit - Terminate the calling thread.
 *
 * @retval: return value to be passed to gc_pthread_join.
 *
 * This function unregisters the thread from the collector and then calls
 * pthread_exit. Use this instead of pthread_exit directly.
 */
void gc_pthread_exit(void *retval) __attribute__((noreturn));

/*
 * gc_register_thread_stack - Manually register a thread's stack.
 *
 * @stack_bottom: the highest address of the thread's stack.
 *
 * This is automatically called for threads created via gc_pthread_create.
 * Use it only if you create a thread by other means (e.g., clone()) and want
 * it to be scanned by the collector. The thread must call this function
 * before allocating any GC-managed memory.
 */
void gc_register_thread_stack(void *stack_bottom);

/*
 * gc_unregister_thread - Manually unregister the calling thread.
 *
 * Should be called before a manually registered thread exits to clean up
 * internal structures. Automatically invoked by gc_pthread_exit.
 */
void gc_unregister_thread(void);

#endif /* GC_MULTITHREAD == 0 */

#if GC_FINALIZING == 0

/* ============================================================================
 * GC-aware pthread synchronization wrappers
 * ============================================================================ */

/*
 * These functions mirror the standard pthread_mutex_* and pthread_cond_*
 * interfaces but allocate the synchronization objects in GC-managed memory.
 * They automatically clean up the underlying pthread objects when no longer
 * reachable (via finalizers). Use them instead of raw pthread functions
 * when the mutex/cond itself may be garbage-collected.
 */

/* Without finalizers, use system types directly to avoid type mismatch warnings. */

typedef pthread_mutex_t gc_mutex_t;
typedef pthread_cond_t gc_cond_t;
typedef atomic_flag gc_atomic_t;

extern gc_atomic_t GC_ATOMIC_FLAG_INIT;
extern gc_mutex_t  GC_PTHREAD_MUTEX_INITIALIZER;

/* Mutex */
int gc_pthread_mutex_init(gc_mutex_t **mutex, const pthread_mutexattr_t *attr);
#define gc_pthread_mutex_lock(m)       pthread_mutex_lock(m)
#define gc_pthread_mutex_unlock(m)     pthread_mutex_unlock(m)
#define gc_pthread_mutex_destroy(m)    pthread_mutex_destroy(m)

int gc_pthread_cond_init(gc_cond_t **cond, const pthread_condattr_t *attr);
#define gc_pthread_cond_wait(c, m)     pthread_cond_wait(c, m)
#define gc_pthread_cond_signal(c)      pthread_cond_signal(c)
#define gc_pthread_cond_broadcast(c)   pthread_cond_broadcast(c)
#define gc_pthread_cond_destroy(c)     pthread_cond_destroy(c)

#else

typedef struct gc_mutex gc_mutex_t;
typedef struct gc_cond gc_cond_t;
typedef struct gc_atomic gc_atomic_t;

extern gc_atomic_t GC_ATOMIC_FLAG_INIT;
extern gc_mutex_t  GC_PTHREAD_MUTEX_INITIALIZER;

/* Mutex */
int gc_pthread_mutex_init(gc_mutex_t **mutex, const pthread_mutexattr_t *attr);
int gc_pthread_mutex_lock(gc_mutex_t *mutex);
int gc_pthread_mutex_unlock(gc_mutex_t *mutex);
int gc_pthread_mutex_destroy(gc_mutex_t *mutex);

/* Condition variable */
int gc_pthread_cond_init(gc_cond_t **cond, const pthread_condattr_t *attr);
int gc_pthread_cond_wait(gc_cond_t *cond, gc_mutex_t *mutex);
int gc_pthread_cond_signal(gc_cond_t *cond);
int gc_pthread_cond_broadcast(gc_cond_t *cond);
int gc_pthread_cond_destroy(gc_cond_t *cond);

#endif /* GC_FINALIZING */

/* ============================================================================
 * Write Barrier for Incremental Marking
 * ============================================================================ */

#if GC_INCREMENTAL == 1

/*
 * GC_WRITE_BARRIER - Notify the collector that a pointer field to be updated.
 *
 * @src:  the object whose field is being written (the container, eg. obj or &obj->field).
 * @val:  the pointer value being stored (the new referent).
 *
 * This macro must be used when storing a pointer into an object's field
 * while incremental marking is active. It prevents the "lost object" problem.
 *
 * Example:
 *   GC_WRITE_BARRIER(parent, child);
 *   parent->next = child;
 *   GC_WRITE_BARRIER(container, new_ptr);
 *   container->ptr = new_ptr;
 * or
 *   GC_WRITE_BARRIER(&parent->next, child);
 *   parent->next = child;
 */
#define GC_WRITE_BARRIER(src, val) do { \
    void *_src = (src); \
    void *_val = (val); \
    if (_src && _val) gc_write_barrier(_src, _val); \
} while(0)

/* Internal write barrier function (do not call directly) */
void gc_write_barrier(void *container_or_field, void *val);

#else

#define GC_WRITE_BARRIER(src, val) ((void)0)

#endif /* GC_INCREMENTAL == 1 */

/*
 * GC_ASSIGN_PTR - pointer assignment with write-barrier protection
 *
 * @lhs: lvalue, pointer field to be assigned with new one (eg. obj->ptr)
 * @rhs: rvalue, new pointer
 *
 * equivalent to:
 *   GC_WRITE_BARRIER(&(lhs), (rhs));
 *   (lhs) = (rhs);
 */
#define GC_ASSIGN_PTR(lhs, rhs) do { \
    GC_WRITE_BARRIER(&(lhs), (rhs)); \
    (lhs) = (rhs); \
} while(0)

/*
 * GC_MALLOC_ROOT(ptr, type)   - allocate memory and add to GC precise roots
 * GC_FREE_ROOT(ptr)           - remove from GC precise roots and set to NULL
 *
 * usage:
 *   GC_MALLOC_ROOT(my_ptr, struct thing);
 *   // use my_ptr ...
 *   GC_FREE_ROOT(my_ptr);
 */
#define GC_MALLOC_ROOT(ptr, type) do { \
    ptr = (type*)gc_malloc(sizeof(type)); \
    if (ptr) gc_add_root(&(ptr)); \
} while(0)

#define GC_FREE_ROOT(ptr) do { \
    if (ptr) { \
        gc_remove_root(&(ptr)); \
        (ptr) = NULL; \
    } \
} while(0)

/*
 * GC_CALLOC_ROOT(ptr, nmemb, type)   - allocate memory and add to GC precise roots
 *
 * usage:
 *   GC_CALLOC_ROOT(my_ptr, nmemb, struct thing);
 *   // use my_ptr ...
 *   GC_FREE_ROOT(my_ptr);
 */
#define GC_CALLOC_ROOT(ptr, nmemb, type) do { \
    ptr = (type*)gc_calloc(nmemb, sizeof(type)); \
    if (ptr) gc_add_root(&(ptr)); \
} while(0)

#ifdef __cplusplus
}
#endif

#endif /* GC_H */

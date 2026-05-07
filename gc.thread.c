/* ============================================================
 * gc.pthr.c - Thread registration, GC‑managed pthread wrappers,
 *             and gc_strdup
 *
 * Implements automatic thread registration via gc_pthread_create,
 * GC‑aware mutex/condition variable wrappers that are allocated in
 * GC memory and can carry finalizers.  Also provides gc_strdup.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 * ============================================================ */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "gc.h"
#define _GNU_SOURCE
//#define DEBUG // Uncomment to debug
#include "log.h"

#if GC_FINALIZING == 1

// finalizers for pthread objects //
static void gc_mutex_finalizer(void *obj, void *data) {
    (void)data;
    gc_mutex_t *m = (gc_mutex_t*)obj;
    pthread_mutex_destroy(&m->lock);
    LOG_DEBUG("finalized mutex %p", m);
}
static void gc_cond_finalizer(void *obj, void *data) {
    (void)data;
    gc_cond_t *c = (gc_cond_t*)obj;
    pthread_cond_destroy(&c->cond);
    LOG_DEBUG("finalized condition variable %p", c);
}

#endif

#include "gc.thread.info.h"

extern gc_thread_info_t *gc_threads;
extern pthread_mutex_t   gc_threads_lock;
extern _Thread_local gc_thread_info_t *gc_tls_self_info;

extern void gc_stw_init(void);


// ---------- Thread registration (multi‑threaded mode) ---------- //
#if GC_MULTITHREAD

extern void gc_update_stack(gc_thread_info_t *t);

void gc_register_thread_stack(void *stack_bottom) {
    gc_thread_info_t *info = malloc(sizeof(struct gc_thread_info));
    if (!info) return;
    info->tid = pthread_self();
    info->stack_bottom = stack_bottom;
    info->stack_top = NULL;
    info->ctx_valid = false;
    pthread_mutex_lock(&gc_threads_lock);
    info->next = gc_threads;
    gc_threads = info;
    pthread_mutex_unlock(&gc_threads_lock);
    gc_update_stack(info);
    gc_tls_self_info = info;
    LOG_DEBUG("registered thread %lu, stack bottom = %p", info->tid, stack_bottom);
}

void gc_unregister_thread(void) {
    pthread_mutex_lock(&gc_threads_lock);
    gc_thread_info_t *prev = NULL, *curr = gc_threads;
    while (curr) {
        if (pthread_equal(curr->tid, pthread_self())) {
            if (prev) prev->next = curr->next;
            else gc_threads = curr->next;
            free(curr);
            LOG_DEBUG("unregistered thread %lu", pthread_self());
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&gc_threads_lock);
    gc_tls_self_info = NULL;
}

static void gc_register_self(void) {
    gc_thread_info_t *info = malloc(sizeof(struct gc_thread_info));
    if (!info) return;
    info->tid = pthread_self();
    pthread_attr_t attr;
    pthread_getattr_np(info->tid, &attr);
    size_t stacksize;
    void *stackaddr;
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
    info->stack_bottom = (char*)stackaddr + stacksize;
    info->stack_top = NULL;
    info->ctx_valid = false;
    pthread_mutex_lock(&gc_threads_lock);
    info->next = gc_threads;
    gc_threads = info;
    pthread_mutex_unlock(&gc_threads_lock);
    gc_tls_self_info = info;
    LOG_DEBUG("auto‑registered thread %lu, stack [%p - %p]", info->tid, stackaddr, info->stack_bottom);
}

typedef struct {
    void *(*start_routine)(void*);
    void *arg;
} gc_thread_wrapper_arg_t;

static void* gc_thread_wrapper(void *arg) {
    gc_thread_wrapper_arg_t *wrap = arg;
    gc_register_self();
    void *ret = wrap->start_routine(wrap->arg);
    gc_unregister_thread();
    free(wrap);
    return ret;
}

int gc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine)(void*), void *arg) {
    gc_stw_init();
    gc_thread_wrapper_arg_t *wrap = malloc(sizeof(*wrap));
    if (!wrap) return ENOMEM;
    wrap->start_routine = start_routine;
    wrap->arg = arg;
    int rc = pthread_create(thread, attr, gc_thread_wrapper, wrap);
    if (rc == 0) LOG_DEBUG("created GC‑managed thread");
    else { free(wrap); LOG_DEBUG("pthread_create failed: %s", strerror(rc)); }
    return rc;
}

int gc_pthread_join(pthread_t thread, void **retval) {
    return pthread_join(thread, retval);
}

void gc_pthread_exit(void *retval) {
    gc_unregister_thread();
    pthread_exit(retval);
}


// ---------- GC‑managed synchronisation primitives (multi‑threaded + finalizers enabled) ---------- //
#if GC_FINALIZING

int gc_pthread_mutex_init(gc_mutex_t **mutex, const pthread_mutexattr_t *attr) {
    gc_mutex_t *m = gc_malloc(sizeof(gc_mutex_t));
    if (!m) return ENOMEM;
    int ret = pthread_mutex_init(&m->lock, attr);
    if (ret != 0) return ret;
    gc_register_finalizer(m, gc_mutex_finalizer, NULL);
    gc_add_root(mutex);            // protect the mutex structure pointer
    gc_add_root(&m->lock);         // protect the inner pthread_mutex_t
    *mutex = m;
    return 0;
}

int gc_pthread_mutex_lock(gc_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    return pthread_mutex_lock(&mutex->lock);
}

int gc_pthread_mutex_unlock(gc_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    return pthread_mutex_unlock(&mutex->lock);
}

int gc_pthread_mutex_destroy(gc_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    int ret = pthread_mutex_destroy(&mutex->lock);
    gc_remove_root(&mutex->lock);
    gc_remove_root(&mutex);        // note: argument is &mutex, the caller's pointer variable
    LOG_DEBUG("explicit destroy mutex %p", mutex);
    return ret;
}

int gc_pthread_cond_init(gc_cond_t **cond, const pthread_condattr_t *attr) {
    gc_cond_t *c = gc_malloc(sizeof(gc_cond_t));
    if (!c) return ENOMEM;
    int ret = pthread_cond_init(&c->cond, attr);
    if (ret != 0) return ret;
    gc_register_finalizer(c, gc_cond_finalizer, NULL);
    gc_add_root(cond);
    gc_add_root(&c->cond);
    *cond = c;
    return 0;
}

int gc_pthread_cond_wait(gc_cond_t *cond, gc_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    return pthread_cond_wait(&cond->cond, &mutex->lock);
}

int gc_pthread_cond_signal(gc_cond_t *cond) {
    if (!cond) return EINVAL;
    return pthread_cond_signal(&cond->cond);
}

int gc_pthread_cond_broadcast(gc_cond_t *cond) {
    if (!cond) return EINVAL;
    return pthread_cond_broadcast(&cond->cond);
}

int gc_pthread_cond_destroy(gc_cond_t *cond) {
    if (!cond) return EINVAL;
    int ret = pthread_cond_destroy(&cond->cond);
    gc_remove_root(&cond->cond);
    gc_remove_root(&cond);
    return ret;
}

#else // GC_FINALIZING

inline int gc_pthread_mutex_init(gc_mutex_t **mutex, const pthread_mutexattr_t *attr) {
    *mutex = gc_malloc(sizeof(gc_mutex_t));
    if (!*mutex) return ENOMEM;
    int ret = pthread_mutex_init(*mutex, attr);
    if (ret != 0) return ret;
    return 0;
}

inline int gc_pthread_cond_init(gc_cond_t **cond, const pthread_condattr_t *attr) {
    *cond = gc_malloc(sizeof(gc_cond_t));
    if (!*cond) return ENOMEM;
    int ret = pthread_cond_init(*cond, attr);
    if (ret != 0) return ret;
    return 0;
}

#endif // GC_FINALIZING

#else // GC_MULTITHREAD

inline int gc_pthread_mutex_init(gc_mutex_t **mutex, const pthread_mutexattr_t *attr) {
    *mutex = gc_malloc(sizeof(gc_mutex_t));
    if (!*mutex) return ENOMEM;
    int ret = pthread_mutex_init(*mutex, attr);
    if (ret != 0) return ret;
    return 0;
}

inline int gc_pthread_cond_init(gc_cond_t **cond, const pthread_condattr_t *attr) {
    *cond = gc_malloc(sizeof(gc_cond_t));
    if (!*cond) return ENOMEM;
    int ret = pthread_cond_init(*cond, attr);
    if (ret != 0) return ret;
    return 0;
}

#endif // GC_MULTITHREAD

inline char *gc_strdup(const char *src) {
    size_t len = strlen(src) + 1;
    char *dst = gc_malloc(len);
    if (dst == NULL) return NULL;
    memcpy(dst, src, len);
    return dst;
}

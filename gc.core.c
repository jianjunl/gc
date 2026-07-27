/* ============================================================
 * gc.core.c - Allocation and collection trigger
 *
 * Implements gc_malloc, gc_calloc, gc_free, and gc_collect.
 * Manages the allocation threshold and decides when to trigger
 * a collection cycle. In incremental mode it also performs a
 * small marking step before satisfying an allocation.
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
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

/* ---------- Configuration ---------- */
#define GC_ALIGNMENT         sizeof(size_t) // usually 8
#define GC_INITIAL_THRESHOLD (4 * 1024 * 1024)
#define GC_HEAP_GROW_FACTOR  2

#if GC_FINALIZING
typedef struct gc_finalizer gc_finalizer_t;
#endif // GC_FINALIZING

#include "gc.block.h"

extern void* gc_os_alloc(size_t size);
extern void gc_os_free(void *ptr, size_t size_hint);

#if GC_INCREMENTAL
extern bool gc_mark_incremental(size_t max_scan_bytes);
extern void gc_mark_roots(void);
#else // GC_INCREMENTAL
extern void gc_mark();
#endif // GC_INCREMENTAL

extern gc_block_t *gc_blocks;

size_t      gc_allocated = 0;
size_t      gc_threshold = GC_INITIAL_THRESHOLD;
extern bool gc_initialized;

#if GC_INCREMENTAL
/* Incremental marking phases */
typedef enum { GC_PHASE_IDLE, GC_PHASE_MARKING, GC_PHASE_SWEEPING } gc_phase_t;
extern gc_phase_t gc_phase;
extern gc_block_t *gc_gray_blocks;
extern pthread_mutex_t gc_gray_lock;
#define GC_MARK_STEP_BYTES  (64 * 1024)
#endif // GC_INCREMENTAL

/* ---------- STW helpers ---------- */

#if GC_MULTITHREAD
extern void gc_stop_all_threads(void);
extern void gc_resume_all_threads(void);
#endif // GC_MULTITHREAD


static inline size_t align_up(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

/* ---------- Allocation functions ---------- */
inline void* gc_malloc(size_t size) {
    return gc_typed_malloc(size, NULL);
}

inline void* gc_calloc(size_t nmemb, size_t size) {
    return gc_typed_calloc(nmemb, size, NULL);
}

void* gc_typed_malloc(size_t size, const gc_type_info_t *type_info) {
    if (!gc_initialized) gc_init();
    size = align_up(size, GC_ALIGNMENT);
    if (size == 0) size = GC_ALIGNMENT;
    LOG_DEBUG("malloc request: %zu bytes (aligned)", size);

#if GC_INCREMENTAL
    if (gc_phase == GC_PHASE_MARKING)
        gc_mark_incremental(GC_MARK_STEP_BYTES);
#endif // GC_INCREMENTAL

    if (gc_allocated + size > gc_threshold) {
        LOG_DEBUG("threshold exceeded, triggering collection");
        gc_collect();
        if (gc_allocated + size > gc_threshold) {
            gc_threshold = (gc_allocated + size) * GC_HEAP_GROW_FACTOR;
            LOG_DEBUG("threshold increased to %zu bytes", gc_threshold);
        }
    }

    void *user_ptr = gc_os_alloc(size);
    if (!user_ptr) return NULL;

    gc_block_t *blk = malloc(sizeof(gc_block_t));
    if (!blk) { gc_os_free(user_ptr, size); return NULL; }

    blk->type_info = type_info;
    blk->ptr       = user_ptr;
    blk->size      = size;
    blk->marked    = false;
#if GC_INCREMENTAL
    blk->marked    = (gc_phase != GC_PHASE_IDLE);
#endif // GC_INCREMENTAL
    blk->next      = gc_blocks;
    gc_blocks      = blk;
    gc_allocated  += size;
    LOG_DEBUG("allocated block %p (user %p) size %zu, total allocated %zu", blk, user_ptr, size, gc_allocated);
    gc_block_insert(user_ptr, blk);
    return user_ptr;
}

void* gc_typed_calloc(size_t nmemb, size_t size, const gc_type_info_t *type_info) {
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) {
        LOG_DEBUG("calloc overflow: %zu * %zu", nmemb, size);
        return NULL;
    }
    void *ptr = gc_typed_malloc(total, type_info);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/*
static bool remove_block_from_list(gc_block_t *blk) {
    gc_block_t *prev = NULL;
    for (gc_block_t *cur = gc_blocks; cur; prev = cur, cur = cur->next) {
        if (cur == blk) {
            if (prev)
                prev->next = cur->next;
            else
                gc_blocks = cur->next;
            return true;
        }
    }
    return false;
}

void gc_free(void *ptr) {
    if (ptr == NULL) return;

    static gc_mutex_t free_mutex = GC_PTHREAD_MUTEX_INITIALIZER;
    gc_pthread_mutex_lock(&free_mutex);

    gc_block_t *blk = gc_find_block(ptr);
    if (blk == NULL) {
        gc_pthread_mutex_unlock(&free_mutex);
        return;
    }

    if (blk->ptr != ptr) {
        gc_pthread_mutex_unlock(&free_mutex);
        return;
    }

    if (!remove_block_from_list(blk)) {
        gc_pthread_mutex_unlock(&free_mutex);
        return;
    }

    gc_block_remove(ptr);

    gc_allocated -= blk->size;

    gc_os_free(blk->ptr, blk->size);
    free(blk);

    gc_pthread_mutex_unlock(&free_mutex);
}
*/
///*
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
void gc_free(void *ptr) {
    (void)ptr; // conservative GC does not free immediately
}
#pragma GCC diagnostic pop
//*/
extern void gc_sweep(void);
#if GC_MULTITHREAD
extern volatile int  gc_stw_suspended_count;
#endif // GC_MULTITHREAD
#if GC_FINALIZING
extern bool gc_skip_finalizers;
#endif // GC_FINALIZING

#if GC_MULTITHREAD
static atomic_flag gc_collect_running = ATOMIC_FLAG_INIT;

static void gc_collect_m(void) {

    if (atomic_flag_test_and_set(&gc_collect_running)) {
        LOG_DEBUG("gc_collect already running in another thread, skipping");
        return;
    }
    static _Thread_local bool in_collect = false;
    if (in_collect) return;
    in_collect = true;

    if (!gc_initialized) gc_init();

#if GC_INCREMENTAL

    if (gc_phase == GC_PHASE_MARKING) {
        LOG_DEBUG("collection requested during marking phase, helping to finish...");
        while (gc_mark_incremental(GC_MARK_STEP_BYTES * 4));
    }

    gc_stop_all_threads();

    gc_phase = GC_PHASE_MARKING;
    //gc_gray_blocks = NULL;
    gc_mark_roots();

    gc_resume_all_threads();

    while (gc_mark_incremental(GC_MARK_STEP_BYTES));

    gc_stop_all_threads();

    gc_phase = GC_PHASE_SWEEPING;
#if GC_FINALIZING
    gc_skip_finalizers = (gc_stw_suspended_count == 0);
#endif // GC_FINALIZING
    gc_sweep();
    gc_phase = GC_PHASE_IDLE;

    gc_resume_all_threads();

    in_collect = false;
    atomic_flag_clear(&gc_collect_running);

#else // GC_INCREMENTAL

    gc_stop_all_threads();
    gc_mark();
#if GC_FINALIZING
    gc_skip_finalizers = (gc_stw_suspended_count == 0);
#endif // GC_FINALIZING
    gc_sweep();
    gc_resume_all_threads();
    in_collect = false;
    atomic_flag_clear(&gc_collect_running);

#endif // GC_INCREMENTAL
}
#endif // GC_MULTITHREAD

void gc_collect(void) {
#if GC_MULTITHREAD
    gc_collect_m();;
#else // GC_MULTITHREAD

    LOG_DEBUG("single-threaded collection");

    if (!gc_initialized) gc_init();

#if GC_INCREMENTAL

    if (gc_phase == GC_PHASE_MARKING) {
        LOG_DEBUG("collection requested during marking phase, helping to finish...");
        while (gc_mark_incremental(GC_MARK_STEP_BYTES * 4));
    }

    gc_phase = GC_PHASE_MARKING;
    gc_gray_blocks = NULL;
    gc_mark_roots();

    while (gc_mark_incremental(GC_MARK_STEP_BYTES));

    gc_phase = GC_PHASE_SWEEPING;
    gc_sweep();
    gc_phase = GC_PHASE_IDLE;

#else // GC_INCREMENTAL

    gc_mark();
    gc_sweep();

#endif // GC_INCREMENTAL
#endif // GC_MULTITHREAD
}

void gc_dump_stats(void) {
    size_t block_count = 0;
    for (gc_block_t *blk = gc_blocks; blk; blk = blk->next) block_count++;
    LOG_NOTE("[GC] allocated: %zu bytes, %zu blocks, threshold: %zu bytes",
            gc_allocated, block_count, gc_threshold);
}

inline char *gc_strdup(const char *src) {
    size_t len = strlen(src) + 1;
    char *dst = gc_malloc(len);
    if (dst == NULL) return NULL;
    memcpy(dst, src, len);
    return dst;
}

void gc_log_ti(const char *name, gc_type_info_t *ti) {
    LOG_NOTE("%s->object_size=%ld", name, ti->object_size);
    LOG_NOTE("%s->n_offsets=%ld", name, ti->n_offsets);
    for (size_t i = 0; i < ti->n_offsets; i++)
        LOG_NOTE("%s->offsets[%ld]=%ld", name, i, ti->offsets[i]);
}

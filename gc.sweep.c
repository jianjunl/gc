/* ============================================================
 * gc.sweep.c - Sweep phase, finalizers and weak references
 *
 * Implements gc_sweep which reclaims unmarked blocks.  When
 * GC_FINALIZING is enabled it also invokes finalizers and clears
 * weak references before freeing memory.
 * ============================================================ */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

// ---------- Configuration ---------- //
#define GC_INITIAL_THRESHOLD (4 * 1024 * 1024)
#define GC_HEAP_GROW_FACTOR  2

#if GC_FINALIZING
typedef struct gc_finalizer gc_finalizer_t;
#endif // GC_FINALIZING

// ---------- Block metadata ---------- //
typedef struct gc_block {
    void               *ptr;
    size_t              size;
    bool                marked;
    struct gc_block    *next;
#if GC_INCREMENTAL
    struct gc_block    *gray_next;   // gray list
#endif // GC_INCREMENTAL
#if GC_FINALIZING
    gc_finalizer_t     *finalizers;  // finalizer list
#endif // GC_FINALIZING
} gc_block_t;

extern void* gc_os_alloc(size_t size);
extern void gc_os_free(void *ptr, size_t size_hint);

extern gc_block_t *gc_blocks;
extern gc_block_t* gc_find_block(void *ptr);

extern size_t      gc_allocated;
extern size_t      gc_threshold;
extern bool        gc_initialized;

#if GC_INCREMENTAL
extern gc_block_t *gc_gray_blocks;
#endif // GC_INCREMENTAL


#if GC_FINALIZING
// weak reference handle
struct gc_weak_ref {
    void               *obj;
    gc_block_t         *block;
    void               (*on_clear)(void*, void*);
    void               *user_data;
    struct gc_weak_ref *next;
};
static gc_weak_ref_t *gc_weak_refs = NULL;
static pthread_mutex_t gc_weak_lock = PTHREAD_MUTEX_INITIALIZER;

// finalizer entry
struct gc_finalizer {
    void (*func)(void*, void*);
    void *user_data;
    struct gc_finalizer *next;
};
#endif // GC_FINALIZING

// ---------- Weak reference API ---------- //
#if GC_FINALIZING

gc_weak_ref_t* gc_make_weak(void *obj, void (*on_clear)(void*, void*), void *user_data) {
    if (!obj) return NULL;
    gc_block_t *blk = gc_find_block(obj);
    if (!blk) return NULL;
    gc_weak_ref_t *ref = malloc(sizeof(gc_weak_ref_t));
    if (!ref) return NULL;
    ref->obj = obj;
    ref->block = blk;
    ref->on_clear = on_clear;
    ref->user_data = user_data;
    pthread_mutex_lock(&gc_weak_lock);
    ref->next = gc_weak_refs;
    gc_weak_refs = ref;
    pthread_mutex_unlock(&gc_weak_lock);
    LOG_DEBUG("created weak reference %p pointing to object %p", ref, obj);
    return ref;
}

void* gc_weak_get(gc_weak_ref_t *ref) {
    if (!ref) return NULL;
    pthread_mutex_lock(&gc_weak_lock);
    gc_block_t *blk = ref->block;
    bool alive = false;
    for (gc_block_t *b = gc_blocks; b; b = b->next)
        if (b == blk) { alive = true; break; }
    pthread_mutex_unlock(&gc_weak_lock);
    return alive ? ref->obj : NULL;
}

void gc_weak_free(gc_weak_ref_t *ref) {
    if (!ref) return;
    pthread_mutex_lock(&gc_weak_lock);
    if (ref->obj == NULL && ref->block == NULL) {
        pthread_mutex_unlock(&gc_weak_lock);
        LOG_DEBUG("weak reference %p already cleared internally, skipping", ref);
        free(ref);
        return;
    }
    gc_weak_ref_t **pp = &gc_weak_refs;
    while (*pp) {
        if (*pp == ref) { *pp = ref->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&gc_weak_lock);
    free(ref);
    LOG_DEBUG("destroyed weak reference %p", ref);
}

static void gc_clear_weak_refs_for_block(gc_block_t *blk) {
    pthread_mutex_lock(&gc_weak_lock);
    gc_weak_ref_t **pp = &gc_weak_refs;
    while (*pp) {
        gc_weak_ref_t *ref = *pp;
        if (ref->block == blk) {
            LOG_DEBUG("clearing weak reference %p for reclaimed object %p", ref, ref->obj);
            if (ref->on_clear) ref->on_clear(ref->obj, ref->user_data);
            *pp = ref->next;
            ref->obj = NULL;
            ref->block = NULL;
            ref->next = NULL;
            continue;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&gc_weak_lock);
}

// ---------- Finalizer API ---------- //
void gc_register_finalizer(void *obj, void (*finalizer)(void*, void*), void *user_data) {
    if (!obj || !finalizer) return;
    gc_block_t *blk = gc_find_block(obj);
    if (!blk) return;
    gc_finalizer_t *fin = malloc(sizeof(struct gc_finalizer));
    if (!fin) return;
    fin->func = finalizer;
    fin->user_data = user_data;
    fin->next = blk->finalizers;
    blk->finalizers = fin;
    LOG_DEBUG("registered finalizer %p for object %p", finalizer, obj);
}

static void gc_invoke_finalizers(gc_block_t *blk) {
    gc_finalizer_t *fin = blk->finalizers;
    while (fin) {
        LOG_DEBUG("invoking finalizer %p for object %p", fin->func, blk->ptr);
        fin->func(blk->ptr, fin->user_data);
        gc_finalizer_t *next = fin->next;
        free(fin);
        fin = next;
    }
    blk->finalizers = NULL;
}

bool gc_skip_finalizers = false;

// sweep phase with finalizers
void gc_sweep(void) {
    gc_block_t *prev = NULL, *curr = gc_blocks;
    size_t freed = 0, freed_blocks = 0;
    while (curr) {
        if (!curr->marked) {
            if (!gc_skip_finalizers) {
                gc_invoke_finalizers(curr);
                gc_clear_weak_refs_for_block(curr);
            }
            gc_os_free(curr->ptr, curr->size);
            gc_allocated -= curr->size;
            freed += curr->size;
            freed_blocks++;
            if (prev) prev->next = curr->next;
            else gc_blocks = curr->next;
            gc_block_t *next = curr->next;
            free(curr);
            curr = next;
        } else {
            curr->marked = false;
            prev = curr;
            curr = curr->next;
        }
    }
    if (freed > 0) {
        LOG_DEBUG("freed %zu bytes (%zu blocks)", freed, freed_blocks);
        if (gc_threshold > GC_INITIAL_THRESHOLD && gc_allocated < gc_threshold / 2) {
            gc_threshold /= GC_HEAP_GROW_FACTOR;
            if (gc_threshold < GC_INITIAL_THRESHOLD) gc_threshold = GC_INITIAL_THRESHOLD;
            LOG_DEBUG("threshold decreased to %zu bytes", gc_threshold);
        }
    } else LOG_DEBUG("no garbage to collect");
    LOG_DEBUG("sweep phase finished, allocated = %zu bytes", gc_allocated);
#if GC_INCREMENTAL
    gc_gray_blocks = NULL;
#endif // GC_INCREMENTAL
}

#else // GC_FINALIZING

// plain sweep phase
void gc_sweep(void) {
    gc_block_t *prev = NULL, *curr = gc_blocks;
    size_t freed = 0, freed_blocks = 0;
    while (curr) {
        if (!curr->marked) {
            gc_os_free(curr->ptr, curr->size);
            gc_allocated -= curr->size;
            freed += curr->size;
            freed_blocks++;
            if (prev) prev->next = curr->next;
            else gc_blocks = curr->next;
            gc_block_t *next = curr->next;
            free(curr);
            curr = next;
        } else {
            curr->marked = false;
            prev = curr;
            curr = curr->next;
        }
    }
    if (freed > 0) {
        LOG_DEBUG("freed %zu bytes (%zu blocks)", freed, freed_blocks);
        if (gc_threshold > GC_INITIAL_THRESHOLD && gc_allocated < gc_threshold / 2) {
            gc_threshold /= GC_HEAP_GROW_FACTOR;
            if (gc_threshold < GC_INITIAL_THRESHOLD) gc_threshold = GC_INITIAL_THRESHOLD;
            LOG_DEBUG("threshold decreased to %zu bytes", gc_threshold);
        }
    } else LOG_DEBUG("no garbage to collect");
    LOG_DEBUG("sweep phase finished, allocated = %zu bytes", gc_allocated);
#if GC_INCREMENTAL
    gc_gray_blocks = NULL;
#endif // GC_INCREMENTAL
}
#endif // GC_FINALIZING

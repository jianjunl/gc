/* ============================================================
 * gc.mark.c - Mark phase (conservative, incremental & non‑incremental)
 *
 * Implements root scanning (data segment, stacks, registers, precise
 * roots) and the transitive closure of reachable objects.  When
 * GC_INCREMENTAL is enabled it provides the gray‑list based
 * incremental marker and the write barrier.
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
#include <ucontext.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

#if GC_FINALIZING
typedef struct gc_finalizer gc_finalizer_t;
#endif // GC_FINALIZING

#include "gc.block.h"

#if GC_INCREMENTAL
// Incremental marking phases //
typedef enum { GC_PHASE_IDLE, GC_PHASE_MARKING, GC_PHASE_SWEEPING } gc_phase_t;
gc_phase_t gc_phase = GC_PHASE_IDLE;
gc_block_t *gc_gray_blocks = NULL;
pthread_mutex_t gc_gray_lock = PTHREAD_MUTEX_INITIALIZER;
#endif // GC_INCREMENTAL

#include "gc.thread.info.h"

gc_thread_info_t *gc_threads = NULL;
pthread_mutex_t   gc_threads_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------- Global state ---------- //
extern gc_block_t *gc_blocks;
extern size_t gc_threshold;

extern char __data_start[];
extern char _end[];
extern void *gc_stack_bottom;

/* Linker‑defined start and end of gc_roots section.
   Use `__attribute__((weak))` in case the section is empty. */
extern void *__start_gc_roots __attribute__((weak));
extern void *__stop_gc_roots  __attribute__((weak));

// ---------- Precise roots ---------- //
#include "gc.root.h"

extern gc_root_t *gc_roots;
extern pthread_mutex_t gc_roots_lock;

// ---------- Low‑level memory allocation ---------- //
extern void* gc_os_alloc(size_t size);
extern void gc_os_free(void *ptr, size_t size_hint);

#if GC_MULTITHREAD 

// ---------- Stop-The-World synchronizing ---------- //
extern _Thread_local gc_thread_info_t *gc_tls_self_info;

#endif // GC_MULTITHREAD

extern gc_block_t* gc_find_block(void *ptr);

// ---------- Pointer validation ---------- //
static bool gc_is_valid_ptr(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    for (gc_block_t *blk = gc_blocks; blk; blk = blk->next) {
        uintptr_t start = (uintptr_t)blk->ptr;
        uintptr_t end   = start + blk->size;
        if (p >= start && p < end) return true;
    }
    return false;
}

extern void* get_stack_top(void);

// ---------- Mark phase (non‑incremental) ---------- //

#if ~GC_INCREMENTAL

static void gc_scan_block_content(gc_block_t *blk);

static void gc_scan_region(void *start, void *end) {
    uintptr_t *p = (uintptr_t*)((uintptr_t)start & ~(sizeof(uintptr_t)-1));
    uintptr_t *limit = (uintptr_t*)end;
    for (; p < limit; p++) {
        uintptr_t val = *p;
        if (val < 0x1000) continue;
        void *candidate = (void*)val;
        if (gc_is_valid_ptr(candidate)) {
            gc_block_t *blk = gc_find_block(candidate);
            if (blk && !blk->marked) {
                blk->marked = true;
                gc_scan_block_content(blk);
            }
        }
    }
}

/* Typed or conservative scanning of a single block */
static void gc_scan_block_content(gc_block_t *blk) {
    if (!blk || !blk->marked) return;

    if (blk->type_info) {
        /* ---- Typed scanning ---- */
        const gc_type_info_t *ti = blk->type_info;
        size_t elem_count = blk->size / ti->object_size;
        char *base = (char*)blk->ptr;
        for (size_t i = 0; i < elem_count; i++) {
            char *elem = base + i * ti->object_size;
            for (size_t j = 0; j < ti->n_offsets; j++) {
                ssize_t off = ti->offsets[j];
                if (off < 0) break;          /* sentinel, though n_offsets guards */
                void **pp = (void**)(elem + off);
                void *cand = *pp;
                if (cand && gc_is_valid_ptr(cand)) {
                    gc_block_t *target = gc_find_block(cand);
                    if (target && !target->marked) {
                        target->marked = true;
                        gc_scan_block_content(target);   /* recursion */
                    }
                }
            }
        }
    } else {
        /* ---- Conservative fallback ---- */
        gc_scan_region(blk->ptr, (char*)blk->ptr + blk->size);
    }
}

/*
 * gc_mark_root_subtree – Mark every pointer reachable from the tree
 * rooted at `root` (including all descendants).  Uses `prev` as an
 * explicit stack, leaving it modified but never depended on after return.
 */
static void gc_mark_root_subtree(gc_root_t *root) {
    gc_root_t *stack = NULL;

    /* push root */
    root->prev = NULL;
    stack = root;

    while (stack) {
        gc_root_t *node = stack;
        stack = node->prev;        /* pop */

        void *ptr = *(node->ptr_addr);
        if (ptr && gc_is_valid_ptr(ptr)) {
            gc_block_t *blk = gc_find_block(ptr);
            if (blk && !blk->marked) {
                blk->marked = true;
                gc_scan_block_content(blk);
            }
        }

        /* Push all children (in any order) */
        gc_root_t *child = node->more;
        while (child) {
            gc_root_t *next_child = child->next;
            child->prev = stack;
            stack = child;
            child = next_child;
        }
    }
}

void gc_mark(void) {
    asm volatile("" ::: "memory");
    LOG_DEBUG("mark phase started");
    gc_scan_region(__data_start, _end);

    // Scan the gc_roots section (if it exists)
    if (&__start_gc_roots != &__stop_gc_roots) {
        void **begin = &__start_gc_roots;
        void **end   = &__stop_gc_roots;
        for (void **p = begin; p < end; p++) {
            void *cand = *p;
            if (cand && gc_is_valid_ptr(cand)) {
                gc_block_t *blk = gc_find_block(cand);
                if (blk && !blk->marked) {
                    blk->marked = true;
                    gc_scan_block_content(blk);
                }
            }
        }
    }

#if ~GC_MULTITHREAD
    void *stack_top = get_stack_top();
    if (gc_stack_bottom && stack_top)
        gc_scan_region(stack_top, gc_stack_bottom);
    ucontext_t ctx;
    if (getcontext(&ctx) == 0)
        gc_scan_region(&ctx, (char*)&ctx + sizeof(ctx));
#else // ~GC_MULTITHREAD
    // Scan stack of current thread (that calling gc_collect)
    if (gc_tls_self_info) {
        void *my_stack_top = get_stack_top();
        if (gc_tls_self_info->stack_bottom && my_stack_top) {
            gc_scan_region(my_stack_top, gc_tls_self_info->stack_bottom);
            LOG_DEBUG("Scanned caller thread's stack [%p - %p]", my_stack_top, gc_tls_self_info->stack_bottom);
        }
    }
    // Scan registers of current thread (that calling gc_collect)
    ucontext_t ctx;
    if (getcontext(&ctx) == 0) {
        gc_scan_region(&ctx, (char*)&ctx + sizeof(ctx));
        LOG_DEBUG("Scanned caller thread's registers");
    }
#endif // ~GC_MULTITHREAD

    pthread_mutex_lock(&gc_threads_lock);
    for (gc_thread_info_t *t = gc_threads; t; t = t->next) {
        if (pthread_equal(t->tid, pthread_self())) continue;
        if (t->stack_top && t->stack_bottom)
            gc_scan_region(t->stack_top, t->stack_bottom);
        if (t->ctx_valid)
            gc_scan_region(&t->suspend_ctx, (char*)&t->suspend_ctx + sizeof(ucontext_t));
    }
    pthread_mutex_unlock(&gc_threads_lock);

    pthread_mutex_lock(&gc_roots_lock);
    for (gc_root_t *r = gc_roots; r; r = r->next)
        gc_mark_root_subtree(r);
    pthread_mutex_unlock(&gc_roots_lock);

    LOG_DEBUG("mark phase finished");
}
#endif // ~GC_INCREMENTAL

// ---------- Incremental mark implementation ---------- //
#if GC_INCREMENTAL

static void gc_gray_push(gc_block_t *blk) {
    if (!blk) return;
    blk->gray_next = gc_gray_blocks;
    gc_gray_blocks = blk;
}

static gc_block_t* gc_gray_pop(void) {
    gc_block_t *blk = gc_gray_blocks;
    if (blk) gc_gray_blocks = blk->gray_next;
    return blk;
}

static void gc_mark_object(gc_block_t *blk) {
    if (!blk->marked) {
        blk->marked = true;
        gc_gray_push(blk);
    }
}

/* Typed or conservative scanning for a single block in incremental mode.
 * Marks any discovered pointers via gc_mark_object (pushes to gray list),
 * NO recursive deep scan. */
static void gc_scan_block_content_incremental(gc_block_t *blk) {
    if (!blk) return;

    if (blk->type_info) {
        const gc_type_info_t *ti = blk->type_info;
        size_t elem_count = blk->size / ti->object_size;
        char *base = (char*)blk->ptr;
        for (size_t i = 0; i < elem_count; i++) {
            char *elem = base + i * ti->object_size;
            for (size_t j = 0; j < ti->n_offsets; j++) {
                ssize_t off = ti->offsets[j];
                if (off < 0) break;          /* sentinel guard */
                void **pp = (void**)(elem + off);
                void *cand = *pp;
                if (cand && gc_is_valid_ptr(cand)) {
                    gc_block_t *target = gc_find_block(cand);
                    if (target) gc_mark_object(target);
                }
            }
        }
    } else {
        // Conservative fallback – original behaviour
        uintptr_t *p = (uintptr_t*)blk->ptr;
        uintptr_t *limit = (uintptr_t*)((char*)blk->ptr + blk->size);
        for (; p < limit; p++) {
            uintptr_t val = *p;
            if (val < 0x1000) continue;
            void *candidate = (void*)val;
            if (gc_is_valid_ptr(candidate)) {
                gc_block_t *target = gc_find_block(candidate);
                if (target) gc_mark_object(target);
            }
        }
    }
}

// Replaces the old gc_scan_block – now just a wrapper
static void gc_scan_block(gc_block_t *blk) {
    gc_scan_block_content_incremental(blk);
}

bool gc_mark_incremental(size_t max_scan_bytes) {
    size_t scanned = 0;
    bool more_work = false;
    pthread_mutex_lock(&gc_gray_lock);
    while (scanned < max_scan_bytes) {
        gc_block_t *blk = gc_gray_pop();
        if (!blk) break;
        size_t block_size = blk->size;
        pthread_mutex_unlock(&gc_gray_lock);
        gc_scan_block(blk);
        scanned += block_size;
        pthread_mutex_lock(&gc_gray_lock);
    }
    more_work = (gc_gray_blocks != NULL);
    pthread_mutex_unlock(&gc_gray_lock);
    LOG_DEBUG("incremental mark: scanned %zu bytes, more work=%d", scanned, more_work);
    return more_work;
}

void gc_mark_roots(void) {
#define MARK_CANDIDATE(ptr) do { \
    void *cand = (ptr); \
    if (gc_is_valid_ptr(cand)) { \
        gc_block_t *blk = gc_find_block(cand); \
        if (blk && !blk->marked) { \
            blk->marked = true; \
            gc_gray_push(blk); \
        } \
    } \
} while(0)

    asm volatile("" ::: "memory");

    // data segment
    uintptr_t *p = (uintptr_t*)__data_start;
    uintptr_t *end = (uintptr_t*)_end;
    for (; p < end; p++) MARK_CANDIDATE((void*)*p);


    // Scan the gc_roots section (if it exists)
    if (&__start_gc_roots != &__stop_gc_roots) {
        void **begin = &__start_gc_roots;
        void **end   = &__stop_gc_roots;
        for (void **p = begin; p < end; p++)
            MARK_CANDIDATE((void*)*p);
    }

#if ~GC_MULTITHREAD
    // main thread stack
    void *stack_top = get_stack_top();
    if (gc_stack_bottom && stack_top)
        for (p = (uintptr_t*)stack_top; p < (uintptr_t*)gc_stack_bottom; p++)
            MARK_CANDIDATE((void*)*p);
#else // ~GC_MULTITHREAD
    // main thread stack of calling thread
    if (gc_tls_self_info) {
        void *my_stack_top = get_stack_top();
        if (gc_tls_self_info->stack_bottom && my_stack_top)
            for (p = (uintptr_t*)my_stack_top; p < (uintptr_t*)gc_tls_self_info->stack_bottom; p++)
                MARK_CANDIDATE((void*)*p);
    }
#endif // ~GC_MULTITHREAD


#if ~GC_MULTITHREAD
    // main thread registers
    ucontext_t ctx;
    if (getcontext(&ctx) == 0)
        for (p = (uintptr_t*)&ctx; p < (uintptr_t*)((char*)&ctx + sizeof(ctx)); p++)
            MARK_CANDIDATE((void*)*p);
#else // ~GC_MULTITHREAD
    // main thread registers of calling thread
    ucontext_t ctx;
    if (getcontext(&ctx) == 0)
        for (p = (uintptr_t*)&ctx; p < (uintptr_t*)((char*)&ctx + sizeof(ctx)); p++)
            MARK_CANDIDATE((void*)*p);
#endif // ~GC_MULTITHREAD

    // suspended threads
    pthread_mutex_lock(&gc_threads_lock);
    for (gc_thread_info_t *t = gc_threads; t; t = t->next) {
        if (pthread_equal(t->tid, pthread_self())) continue;
        if (t->stack_top && t->stack_bottom)
            for (p = t->stack_top; p < (uintptr_t*)t->stack_bottom; p++)
                MARK_CANDIDATE((void*)*p);
        if (t->ctx_valid)
            for (p = (uintptr_t*)&t->suspend_ctx;
                 p < (uintptr_t*)((char*)&t->suspend_ctx + sizeof(ucontext_t)); p++)
                MARK_CANDIDATE((void*)*p);
    }
    pthread_mutex_unlock(&gc_threads_lock);

    // precise roots
    pthread_mutex_lock(&gc_roots_lock);
    for (gc_root_t *r = gc_roots; r; r = r->next) {
        gc_root_t *stack = NULL;
        r->prev = NULL;
        stack = r;
        while (stack) {
            gc_root_t *node = stack;
            stack = node->prev;
            void *root_ptr = *(node->ptr_addr);
            MARK_CANDIDATE(root_ptr);
            gc_root_t *child = node->more;
            while (child) {
                gc_root_t *next_child = child->next;
                child->prev = stack;
                stack = child;
                child = next_child;
            }
        }
    }
    pthread_mutex_unlock(&gc_roots_lock);
#undef MARK_CANDIDATE
}

/* write barrier implementation (unified version)
 * @container: containing object or address of its field (eg. obj or &obj->field)
 * @val:       new target pointer to be assigned
 */
void gc_write_barrier(void *container, void *val) {
    if (gc_phase != GC_PHASE_MARKING) return;

    gc_block_t *container_blk = gc_find_block(container);
    if (!container_blk || !container_blk->marked) {
        return;  // container not black, no need further processing
    }

    gc_block_t *val_blk = gc_find_block(val);
    if (!val_blk || val_blk->marked) return;

    LOG_DEBUG("write barrier: marking container object %p (white object %p to be assigning)", container, val);
    pthread_mutex_lock(&gc_gray_lock);
    gc_mark_object(val_blk);
    pthread_mutex_unlock(&gc_gray_lock);
}

#endif // GC_INCREMENTAL

/* ============================================================
 * gc.block.c - Block management and lookup
 *
 * Now includes a simple chain‑hash table that maps a user data
 * pointer directly to its gc_block_t header.  This eliminates the
 * previous O(n) list traversal from gc_find_block_by_ptr.
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

/* ---------- Global block list (still maintained) ---------- */
gc_block_t *gc_blocks = NULL;

/* ---------- Hash table for fast user‑pointer → block lookup ---------- */
#define GC_HASH_SIZE  1024               // must be a power of two
#define GC_HASH_MASK  (GC_HASH_SIZE - 1)

typedef struct gc_hash_entry {
    void                 *ptr;       /* user data pointer (key)       */
    gc_block_t           *block;     /* associated block header       */
    struct gc_hash_entry *next;
} gc_hash_entry_t;

static gc_hash_entry_t *gc_hash_table[GC_HASH_SIZE];
static pthread_rwlock_t gc_hash_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Simple but reasonable hash for pointers */
static inline unsigned int gc_hash_ptr(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    /* Mix upper and lower bits */
    return (unsigned int)((p >> 4) ^ (p >> 12)) & GC_HASH_MASK;
}

/*
 * gc_hash_insert – add a mapping from user pointer to block.
 * Called from gc_malloc() right after a new block is created.
 */
void gc_hash_insert(void *ptr, gc_block_t *blk) {
    unsigned int idx = gc_hash_ptr(ptr);
    gc_hash_entry_t *e = malloc(sizeof(*e));
    if (!e) return;   // extremely unlikely, but avoid crashing
    e->ptr   = ptr;
    e->block = blk;
    pthread_rwlock_wrlock(&gc_hash_lock);
    e->next = gc_hash_table[idx];
    gc_hash_table[idx] = e;
    pthread_rwlock_unlock(&gc_hash_lock);
}

/*
 * gc_hash_remove – delete the mapping for a user pointer.
 * Called from gc_sweep() just before a block is freed.
 */
void gc_hash_remove(void *ptr) {
    if (!ptr) return;
    unsigned int idx = gc_hash_ptr(ptr);
    pthread_rwlock_wrlock(&gc_hash_lock);
    gc_hash_entry_t **pp = &gc_hash_table[idx];
    while (*pp) {
        gc_hash_entry_t *e = *pp;
        if (e->ptr == ptr) {
            *pp = e->next;
            free(e);
            break;
        }
        pp = &e->next;
    }
    pthread_rwlock_unlock(&gc_hash_lock);
}

/*
 * gc_find_block - locate a block header by its exact address.
 * Kept for future use; currently not used internally.
 */
gc_block_t* gc_find_block(void *blk) {
    for (gc_block_t *node = gc_blocks; node; node = node->next) {
        if ((void *)node == blk) return node;
    }
    return NULL;
}

/*
 * gc_find_block_by_ptr - O(1) lookup of the block that contains a user pointer.
 * Uses the hash table exclusively – no list traversal needed.
 */
gc_block_t* gc_find_block_by_ptr(void *ptr) {
    if (!ptr) return NULL;
    unsigned int idx = gc_hash_ptr(ptr);
    pthread_rwlock_rdlock(&gc_hash_lock);
    gc_hash_entry_t *e = gc_hash_table[idx];
    while (e) {
        if (e->ptr == ptr) {
            gc_block_t *blk = e->block;
            pthread_rwlock_unlock(&gc_hash_lock);
            return blk;
        }
        e = e->next;
    }
    pthread_rwlock_unlock(&gc_hash_lock);
    return NULL;
}

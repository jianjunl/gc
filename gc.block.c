/* ============================================================
 * gc.block.c - Block management and lookup
 *
 * Now includes a simple chain‑hash table that maps a user data
 * pointer directly to its gc_block_t header.  This eliminates the
 * previous O(n) list traversal from gc_find_block.
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

// ---------- default pointer validating hook ---------- //
static void* default_validate(void *ptr) {
    return ptr;   // default: return original pointer directly
}

gc_validate_fn gc_validate_hook = default_validate;

void gc_set_validate_hook(gc_validate_fn fn) {
    if (fn) {
        gc_validate_hook = fn;
    }
}

// ---------- Global block list (still maintained) ---------- //
gc_block_t *gc_blocks = NULL;

static pthread_rwlock_t gc_block_rwlock = PTHREAD_RWLOCK_INITIALIZER;

//#define GC_BLOCK_HASH

#ifndef GC_BLOCK_HASH

#ifndef GC_BLOCK_AMAP

#include "robin/robin_table.h"

static robin_table_t *rt = NULL;

void gc_block_remove(void *ptr) {
    pthread_rwlock_wrlock(&gc_block_rwlock);
    uintptr_t key = (uintptr_t)ptr;
    pthread_rwlock_wrlock(&gc_block_rwlock);
    if (rt) {
        robin_table_del(rt, &key, sizeof(key));
    }
    pthread_rwlock_unlock(&gc_block_rwlock);
}

void gc_block_insert(void *ptr, gc_block_t *blk) {
    (void)ptr;
    pthread_rwlock_wrlock(&gc_block_rwlock);
    if (!rt) rt = robin_table_create(1024, robin_table_rapidhash, RT_RAPID_SEED, NULL);
    if (rt) {
        robin_table_put(rt, &(blk->ptr), sizeof(blk->ptr), blk);
    }
    pthread_rwlock_unlock(&gc_block_rwlock);
}

gc_block_t* gc_find_block(void *ptr) {
    if (!ptr) return NULL;

    // Apply user defined pointer validate hook (NaN boxing decode)
    ptr = gc_validate_hook(ptr);
    if (!ptr) return NULL;

    uintptr_t key = (uintptr_t)ptr;
    gc_block_t *blk = NULL;
    pthread_rwlock_rdlock(&gc_block_rwlock);
    if (rt) {
        blk = robin_table_get(rt, &key, sizeof(key));
    }
    pthread_rwlock_unlock(&gc_block_rwlock);
    return blk;
}

#else // GC_BLOCK_AMAP

#include "art/artmap.h"

static AMAP t = {.root = 0, .conf = &(amap_conf){malloc, calloc, free}};

void gc_block_remove(void *ptr) {
    pthread_rwlock_wrlock(&gc_block_rwlock);
    amap_delete_with_shrink(&t, (uint64_t)ptr);
    pthread_rwlock_unlock(&gc_block_rwlock);
}

void gc_block_insert(void *ptr, gc_block_t *blk) {
    pthread_rwlock_wrlock(&gc_block_rwlock);
    amap_insert(&t, (uint64_t)ptr, blk);
    pthread_rwlock_unlock(&gc_block_rwlock);
}

gc_block_t* gc_find_block(void *ptr) {
    if (!ptr) return NULL;

    // Apply user defined pointer validate hook (NaN boxing decode)
    ptr = gc_validate_hook(ptr);
    if (!ptr) return NULL;

    pthread_rwlock_rdlock(&gc_block_rwlock);
    gc_block_t *blk = (gc_block_t *)amap_search(&t, (uint64_t)ptr);
    pthread_rwlock_unlock(&gc_block_rwlock);
    return blk;
}

#endif // GC_BLOCK_AMAP

#else // GC_BLOCK_HASH

// ---------- Hash table for fast user‑pointer → block lookup ---------- //
#define GC_HASH_SIZE  1024*4             // must be a power of two
#define GC_HASH_MASK  (GC_HASH_SIZE - 1)

typedef struct gc_hash_entry {
    void                 *ptr;       // user data pointer (key)
    gc_block_t           *block;     // associated block header
    struct gc_hash_entry *next;
} gc_hash_entry_t;

static gc_hash_entry_t *gc_hash_table[GC_HASH_SIZE];

// Simple but reasonable hash for pointers
static inline unsigned int gc_hash_ptr(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    // Mix upper and lower bits
    return (unsigned int)((p >> 4) ^ (p >> 12)) & GC_HASH_MASK;
}

// gc_block_insert – add a mapping from user pointer to block.
// Called from gc_malloc() right after a new block is created.
void gc_block_insert(void *ptr, gc_block_t *blk) {
    unsigned int idx = gc_hash_ptr(ptr);
    gc_hash_entry_t *e = malloc(sizeof(*e));
    if (!e) return;   // extremely unlikely, but avoid crashing
    e->ptr   = ptr;
    e->block = blk;
    pthread_rwlock_wrlock(&gc_block_rwlock);
    e->next = gc_hash_table[idx];
    gc_hash_table[idx] = e;
    pthread_rwlock_unlock(&gc_block_rwlock);
}

// gc_block_remove – delete the mapping for a user pointer.
// Called from gc_sweep() just before a block is freed.
void gc_block_remove(void *ptr) {
    if (!ptr) return;
    unsigned int idx = gc_hash_ptr(ptr);
    pthread_rwlock_wrlock(&gc_block_rwlock);
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
    pthread_rwlock_unlock(&gc_block_rwlock);
}

// gc_find_block - O(1) lookup of the block that contains a user pointer.
// Uses the hash table exclusively – no list traversal needed.
gc_block_t* gc_find_block(void *ptr) {
    if (!ptr) return NULL;

    // Apply user defined pointer validate hook (NaN boxing decode)
    ptr = gc_validate_hook(ptr);
    if (!ptr) return NULL;

    unsigned int idx = gc_hash_ptr(ptr);
    pthread_rwlock_rdlock(&gc_block_rwlock);
    gc_hash_entry_t *e = gc_hash_table[idx];
    while (e) {
        if (e->ptr == ptr) {
            gc_block_t *blk = e->block;
            pthread_rwlock_unlock(&gc_block_rwlock);
            return blk;
        }
        e = e->next;
    }
    pthread_rwlock_unlock(&gc_block_rwlock);
    return NULL;
}

#endif // GC_BLOCK_HASH

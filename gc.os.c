/* ============================================================
 * gc.os.c - Low‑level OS memory management (mmap‑based)
 *
 * Provides gc_os_alloc and gc_os_free.  Uses mmap/munmap directly
 * and implements an optional free‑list (GC_FREELIST) to cache small
 * blocks and reduce system calls.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 * ============================================================ */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

// ---------- Free‑list configuration ---------- //
#if GC_FREELIST
#define GC_FREELIST_MAX_CACHED   (16 * 1024 * 1024)
#define GC_FREELIST_BUCKETS      8

static const size_t gc_freelist_bucket_sizes[GC_FREELIST_BUCKETS] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};

typedef struct gc_freelist_node {
    struct gc_freelist_node *next;
    size_t                   block_size;
} gc_freelist_node_t;

static struct {
    gc_freelist_node_t *head;
    size_t              cached_bytes;
} gc_freelist[GC_FREELIST_BUCKETS];

static size_t gc_freelist_total_cached = 0;
static pthread_mutex_t gc_freelist_lock = PTHREAD_MUTEX_INITIALIZER;

static int gc_freelist_find_bucket(size_t size) {
    for (int i = 0; i < GC_FREELIST_BUCKETS; i++)
        if (size <= gc_freelist_bucket_sizes[i]) return i;
    return -1;
}

#endif // GC_FREELIST

// ---------- Low‑level memory allocation ---------- //
void* gc_os_alloc(size_t size) {
#if GC_FREELIST
    int bucket = gc_freelist_find_bucket(size);
    if (bucket >= 0) {
        size_t bucket_size = gc_freelist_bucket_sizes[bucket];
        size_t total_alloc = bucket_size + sizeof(size_t);
        pthread_mutex_lock(&gc_freelist_lock);
        gc_freelist_node_t *node = gc_freelist[bucket].head;
        if (node) {
            gc_freelist[bucket].head = node->next;
            gc_freelist[bucket].cached_bytes -= bucket_size;
            gc_freelist_total_cached -= bucket_size;
            pthread_mutex_unlock(&gc_freelist_lock);
            node->next = NULL;   // prevent stale pointer from being scanned by GC
            void *user_ptr = (char*)node + sizeof(size_t);
            LOG_DEBUG("freelist hit: bucket %d, size %zu, raw %p -> user %p", bucket, bucket_size, node, user_ptr);
            return user_ptr;
        }
        pthread_mutex_unlock(&gc_freelist_lock);
        void *raw = mmap(NULL, total_alloc, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) return NULL;
        *(size_t*)raw = bucket_size;
        void *user_ptr = (char*)raw + sizeof(size_t);
        LOG_DEBUG("freelist miss: bucket %d, mmap %zu bytes, raw %p -> user %p", bucket, total_alloc, raw, user_ptr);
        return user_ptr;
    } else {
        size_t total_alloc = size + sizeof(size_t);
        void *raw = mmap(NULL, total_alloc, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) return NULL;
        *(size_t*)raw = size;
        void *user_ptr = (char*)raw + sizeof(size_t);
        LOG_DEBUG("large allocation: mmap %zu bytes, raw %p -> user %p", total_alloc, raw, user_ptr);
        return user_ptr;
    }
#else // GC_FREELIST
    size_t total_alloc = size + sizeof(size_t);
    void *raw = mmap(NULL, total_alloc, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    *(size_t*)raw = size;
    void *user_ptr = (char*)raw + sizeof(size_t);
    LOG_DEBUG("large allocation: mmap %zu bytes, raw %p -> user %p", total_alloc, raw, user_ptr);
    return user_ptr;
#endif // GC_FREELIST
}

void gc_os_free(void *ptr, size_t size_hint) {
    if (!ptr) return;
    (void)size_hint;
    void *raw = (char*)ptr - sizeof(size_t);
    size_t actual_size = *(size_t*)raw;
#if GC_FREELIST
    int bucket = gc_freelist_find_bucket(actual_size);
    if (bucket >= 0) {
        size_t bucket_size = gc_freelist_bucket_sizes[bucket];
        size_t total_alloc = bucket_size + sizeof(size_t);
        pthread_mutex_lock(&gc_freelist_lock);
        if (gc_freelist_total_cached + bucket_size <= GC_FREELIST_MAX_CACHED) {
            gc_freelist_node_t *node = (gc_freelist_node_t*)raw;
            node->block_size = bucket_size;
            node->next = gc_freelist[bucket].head;
            gc_freelist[bucket].head = node;
            gc_freelist[bucket].cached_bytes += bucket_size;
            gc_freelist_total_cached += bucket_size;
            pthread_mutex_unlock(&gc_freelist_lock);
            LOG_DEBUG("freed user %p (raw %p, size %zu) -> cached in bucket %d, total cached %zu",
                ptr, raw, bucket_size, bucket, gc_freelist_total_cached);
        } else {
            pthread_mutex_unlock(&gc_freelist_lock);
            LOG_DEBUG("cache full, munmap raw %p (size %zu)", raw, total_alloc);
            munmap(raw, total_alloc);
        }
    } else {
        size_t total_alloc = actual_size + sizeof(size_t);
        LOG_DEBUG("large free: user %p, raw %p, size %zu, munmap %zu bytes", ptr, raw, actual_size, total_alloc);
        munmap(raw, total_alloc);
    }
#else // GC_FREELIST
    size_t total_alloc = actual_size + sizeof(size_t);
    LOG_DEBUG("large free: user %p, raw %p, size %zu, munmap %zu bytes", ptr, raw, actual_size, total_alloc);
    munmap(raw, total_alloc);
#endif // GC_FREELIST
}

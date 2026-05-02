/* ============================================================
 * gc.root.c - Precise root management
 *
 * Implements gc_add_root, gc_remove_root, gc_(un)root_cleanup, and
 * gc_root_malloc.  Precise roots protect specific pointer variables
 * from being missed by conservative stack scanning.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 * ============================================================ */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

// ---------- Precise roots ---------- //
typedef struct gc_root {
    void              **ptr_addr;
    struct gc_root     *next;
} gc_root_t;

gc_root_t *gc_roots = NULL;

pthread_mutex_t gc_roots_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------- Precise root management ---------- //
void gc_add_root(void *ptr) {
    gc_root_t *root = malloc(sizeof(gc_root_t));
    if (!root) return;
    root->ptr_addr = (void**)ptr;
    pthread_mutex_lock(&gc_roots_lock);
    root->next = gc_roots;
    gc_roots = root;
    pthread_mutex_unlock(&gc_roots_lock);
    LOG_DEBUG("added precise root %p (points to %p)", ptr, *(void**)ptr);
}

void gc_remove_root(void *ptr) {
    pthread_mutex_lock(&gc_roots_lock);
    gc_root_t *prev = NULL, *curr = gc_roots;
    while (curr) {
        if (curr->ptr_addr == (void**)ptr) {
            if (prev) prev->next = curr->next;
            else gc_roots = curr->next;
            free(curr);
            LOG_DEBUG("removed precise root %p", ptr);
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&gc_roots_lock);
}

void gc_root_cleanup(void **ptr_addr) {
    if (ptr_addr) gc_remove_root(ptr_addr);
}

/* ============================================================
 * gc.root.c - Precise root management (list-based, non‑recursive)
 *
 * Implements gc_add_root, gc_remove_root, gc_root_add,
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
// #define DEBUG
#include "log.h"
#include "gc.root.h"

gc_root_t *gc_roots = NULL;
pthread_mutex_t gc_roots_lock = PTHREAD_MUTEX_INITIALIZER;

/* 添加一个根（指向指针变量的地址） */
void gc_add_root(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&gc_roots_lock);
    for (gc_root_t *r = gc_roots; r; r = r->next) {
        if (r->ptr_addr == (void**)ptr) {
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
    }
    gc_root_t *new_root = malloc(sizeof(gc_root_t));
    if (new_root) {
        new_root->ptr_addr = (void **)ptr;
        new_root->next = gc_roots;
        gc_roots = new_root;
        LOG_DEBUG("added root %p", ptr);
    }
    pthread_mutex_unlock(&gc_roots_lock);
}

/* 移除一个根（匹配 ptr_addr 字段） */
void gc_remove_root(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&gc_roots_lock);
    gc_root_t **indirect = &gc_roots;
    while (*indirect) {
        gc_root_t *curr = *indirect;
        if (curr->ptr_addr == (void **)ptr) {
            *indirect = curr->next;
            free(curr);
            LOG_DEBUG("removed root %p", ptr);
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
        indirect = &curr->next;
    }
    pthread_mutex_unlock(&gc_roots_lock);
}

inline void gc_root_cleanup(void *ptraddr) {
    if (ptraddr) gc_remove_root(ptraddr);
}

inline void gc_auto_root_cleanup(void **ptr) {
    if (ptr && *ptr) {
        gc_remove_root(*ptr);
    }
}

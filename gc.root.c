/* ============================================================
 * gc.root.c - Precise root management (tree-based, non‑recursive)
 *
 * Implements gc_add_root, gc_remove_root, gc_root_add,
 * gc_root_add_bulk, gc_root_assign and helpers.  The internal
 * tree is "first‑child / next‑sibling".  The `prev` field is
 * used as a temporary stack pointer in non‑recursive traversals.
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

/* ------------------------------------------------------------------ */
typedef struct gc_root {
    void              **ptr_addr;   /* address of the pointer variable */
    struct gc_root     *next;       /* sibling in the same child list  */
    struct gc_root     *more;       /* first child (head of child list) */
    struct gc_root     *prev;       /* temporary stack link (all phases) */
} gc_root_t;

gc_root_t *gc_roots = NULL;
pthread_mutex_t gc_roots_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Non‑recursive internal helpers                                     */
/* ------------------------------------------------------------------ */

/*
 * free_subtree – free all nodes in the tree rooted at `node`.
 * Uses `prev` to build an explicit stack, so no recursion is needed.
 * After the call, the whole subtree is deallocated.
 */
static void free_subtree(gc_root_t *node) {
    if (!node) return;
    gc_root_t *stack = NULL;
    node->prev = NULL;
    stack = node;
    while (stack) {
        gc_root_t *cur = stack;
        stack = cur->prev;
        /* push all children onto the stack */
        gc_root_t *child = cur->more;
        while (child) {
            gc_root_t *next_child = child->next;
            child->prev = stack;
            stack = child;
            child = next_child;
        }
        free(cur);
    }
}

/*
 * find_node_by_ptr_addr – search the subtree rooted at `root` for a node
 * whose `ptr_addr` exactly matches `target`.  Returns the node or NULL.
 * Uses a `prev`-based stack, so it is non‑recursive.  The `prev` fields
 * are overwritten during the search but are not needed afterwards.
 */
static gc_root_t *find_node_by_ptr_addr(gc_root_t *root, void **target) {
    if (!root) return NULL;
    gc_root_t *stack = NULL;
    root->prev = NULL;
    stack = root;
    while (stack) {
        gc_root_t *cur = stack;
        stack = cur->prev;
        if (cur->ptr_addr == target) return cur;
        /* push children */
        gc_root_t *child = cur->more;
        while (child) {
            gc_root_t *next_child = child->next;
            child->prev = stack;
            stack = child;
            child = next_child;
        }
    }
    return NULL;
}

/*
 * gc_root_add_child_to_parent – traverse the subtree rooted at `node`
 * and, for every node whose *ptr_addr equals `parent`, insert a new
 * child node for `child_addr` at the head of its `more` list.
 * Returns true if at least one insertion was performed.
 * Non‑recursive: uses a `prev`‑based stack.
 */
static bool gc_root_add_child_to_parent(gc_root_t *node,
                                        void *parent, void *child_addr) {
    if (!node) return false;
    bool inserted = false;
    gc_root_t *stack = NULL;
    node->prev = NULL;
    stack = node;
    while (stack) {
        gc_root_t *cur = stack;
        stack = cur->prev;
        if (*(cur->ptr_addr) == parent) {
            gc_root_t *new_child = malloc(sizeof(gc_root_t));
            if (new_child) {
                new_child->ptr_addr = (void **)child_addr;
                new_child->more = NULL;
                new_child->prev = NULL;
                new_child->next = cur->more;   /* insert at front */
                cur->more = new_child;
            }
            inserted = true;
        }
        /* push children */
        gc_root_t *child = cur->more;
        while (child) {
            gc_root_t *next_child = child->next;
            child->prev = stack;
            stack = child;
            child = next_child;
        }
    }
    return inserted;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void gc_add_root(void *ptr) {
    gc_root_add(ptr, NULL);
}

void gc_root_add(void *parent, void *child) {
    if (!parent) {
        LOG_FATAL("gc_root_add: parent is NULL");
        return;
    }
    pthread_mutex_lock(&gc_roots_lock);

    if (!child) {
        /* simple root – original behaviour */
        gc_root_t *new_root = malloc(sizeof(gc_root_t));
        if (new_root) {
            new_root->ptr_addr = (void **)parent;
            new_root->more = NULL;
            new_root->prev = NULL;
            new_root->next = gc_roots;
            gc_roots = new_root;
            LOG_DEBUG("added root %p", parent);
        }
    } else {
        bool found = false;
        for (gc_root_t *r = gc_roots; r; r = r->next) {
            if (gc_root_add_child_to_parent(r, parent, child))
                found = true;
        }
        if (!found) {
            /* None of the existing nodes has *ptr_addr == parent.
               Register child as a new top‑level root. */
            gc_root_t *new_root = malloc(sizeof(gc_root_t));
            if (new_root) {
                new_root->ptr_addr = (void **)child;
                new_root->more = NULL;
                new_root->prev = NULL;
                new_root->next = gc_roots;
                gc_roots = new_root;
                LOG_DEBUG("gc_root_add: parent %p not found, "
                          "added child %p as root", parent, child);
            }
        }
    }

    pthread_mutex_unlock(&gc_roots_lock);
}

void gc_root_add_bulk(void *base, size_t nmemb, size_t size) {
    if (!base || nmemb == 0) return;

    pthread_mutex_lock(&gc_roots_lock);

    /* Is there already a root node for `base`? */
    gc_root_t *base_node = NULL;
    for (gc_root_t *r = gc_roots; r && !base_node; r = r->next)
        base_node = find_node_by_ptr_addr(r, (void **)base);

    if (base_node) {
        /* Replace existing children */
        free_subtree(base_node->more);
        base_node->more = NULL;
    } else {
        /* Create a new top‑level root for `base` */
        base_node = malloc(sizeof(gc_root_t));
        if (!base_node) {
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
        base_node->ptr_addr = (void **)base;
        base_node->more = NULL;
        base_node->prev = NULL;
        base_node->next = gc_roots;
        gc_roots = base_node;
    }

    /* Build a linked list (reversed order) of child nodes */
    gc_root_t *head = NULL;
    for (size_t i = 0; i < nmemb; i++) {
        gc_root_t *child = malloc(sizeof(gc_root_t));
        if (!child) break;
        child->ptr_addr = (void **)((char *)base + i * size);
        child->more = NULL;
        child->prev = NULL;
        child->next = head;
        head = child;
    }
    base_node->more = head;   /* head is the first child */

    pthread_mutex_unlock(&gc_roots_lock);
    LOG_DEBUG("gc_root_add_bulk: base=%p, nmemb=%zu", base, nmemb);
}

void gc_root_assign(void *old, void *new) {
    if (!old || !new) return;

    pthread_mutex_lock(&gc_roots_lock);

    /* Traverse the entire root tree (top-level + all children) and
       update matching nodes.  We use a prev‑based stack for the walk. */
    for (gc_root_t *r = gc_roots; r; r = r->next) {
        gc_root_t *stack = NULL;
        r->prev = NULL;
        stack = r;
        while (stack) {
            gc_root_t *cur = stack;
            stack = cur->prev;
            if (cur->ptr_addr == old) {
                /* change the address */
                cur->ptr_addr = (void **)new;
                /* free its entire subtree */
                free_subtree(cur->more);
                cur->more = NULL;
            }
            /* push children */
            gc_root_t *child = cur->more;
            while (child) {
                gc_root_t *next_child = child->next;
                child->prev = stack;
                stack = child;
                child = next_child;
            }
        }
    }

    pthread_mutex_unlock(&gc_roots_lock);
    LOG_DEBUG("gc_root_assign: old=%p new=%p", old, new);
}

/*
 * Remove the node with `ptr_addr == target` (and free its subtree)
 * from the child list pointed to by `head_ref`.
 * Returns true if found and removed.
 */
static bool remove_from_children(gc_root_t **head_ref, void *target) {
    gc_root_t **indirect = head_ref;
    while (*indirect) {
        gc_root_t *curr = *indirect;
        if (curr->ptr_addr == target) {
            *indirect = curr->next;          /* unlink */
            free_subtree(curr);
            return true;
        }
        /* check deeper children of this node */
        if (remove_from_children(&curr->more, target))
            return true;
        indirect = &curr->next;
    }
    return false;
}

void gc_remove_root(void *ptr) {
    pthread_mutex_lock(&gc_roots_lock);

    /* 1) check top‑level list */
    gc_root_t **indirect = &gc_roots;
    while (*indirect) {
        gc_root_t *node = *indirect;
        if (node->ptr_addr == ptr) {
            *indirect = node->next;
            free_subtree(node);
            LOG_DEBUG("removed root %p (top-level)", ptr);
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
        /* 2) search inside children of this top‑level node */
        if (remove_from_children(&node->more, ptr)) {
            LOG_DEBUG("removed root %p (child)", ptr);
            pthread_mutex_unlock(&gc_roots_lock);
            return;
        }
        indirect = &node->next;
    }

    pthread_mutex_unlock(&gc_roots_lock);
}

void gc_root_cleanup(void **ptr_addr) {
    if (ptr_addr) gc_remove_root(ptr_addr);
}

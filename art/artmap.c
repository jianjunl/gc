
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "artmap.h"

typedef enum {
    LEAF,
    NODE4,
    NODE16,
    NODE48,
    NODE256
} AMAP_NODE;

struct amap_node {
    union {
        struct { uint8_t keys[4]; struct amap_node *child[4]; } n4;
        struct { uint8_t keys[16]; struct amap_node *child[16]; } n16;
        struct { uint8_t index[256]; struct amap_node *child[48]; } n48;
        struct { struct amap_node *child[256]; } n256;
        struct { uint8_t key[AMAP_KLEN]; void *val; } leaf;
    };
    uint32_t children;
    AMAP_NODE type;
};

static amap_conf default_conf = (amap_conf) {
    .malloc = malloc,
    .calloc = calloc,
    .free   = free
}; 

AMAP* amap_create(amap_conf *c) {
    if (!c) c = &default_conf;
    if (!c->malloc) c->malloc = malloc;
    if (!c->calloc) c->calloc = calloc;
    if (!c->free) c->free = free;
    AMAP *tree = (AMAP*)c->malloc(sizeof(AMAP));
    if (!tree) return NULL;
    tree->conf = c;
    tree->root = 0;
    return tree;
}

static inline uint8_t key_byte(uint64_t k, int depth) {
    return (k >> (56 - 8*depth)) & 0xFF;
}

static inline uint64_t key_from_bytes(const uint8_t bytes[8]) {
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | bytes[i];
    return k;
}

static amap_node *alloc_node(AMAP *a, AMAP_NODE t) {
    amap_node *n = a->conf->calloc(1, sizeof(amap_node));
    n->type = t;
    return n;
}

static void free_node(AMAP *a, amap_node *n) {
    if (n) a->conf->free(n);
}

static amap_node *make_leaf(AMAP *a, uint64_t k, void *v) {
    amap_node *n = alloc_node(a, LEAF);
    for (int i = 0; i < 8; i++) n->leaf.key[i] = key_byte(k, i);
    n->leaf.val = v;
    return n;
}

static amap_node *child(amap_node *n, uint8_t c) {
    switch (n->type) {
    case NODE4:
        for (unsigned i = 0; i < n->children; i++)
            if (n->n4.keys[i] == c) return n->n4.child[i];
        break;
    case NODE16:
        for (unsigned i = 0; i < n->children; i++)
            if (n->n16.keys[i] == c) return n->n16.child[i];
        break;
    case NODE48:
        if (n->n48.index[c]) return n->n48.child[n->n48.index[c]-1];
        break;
    case NODE256:
        return n->n256.child[c];
    default: break;
    }
    return NULL;
}

static void replace_child(amap_node *n, uint8_t c, amap_node *old, amap_node *new) {
    switch (n->type) {
    case NODE4:
        for (unsigned i = 0; i < n->children; i++)
            if (n->n4.keys[i] == c && n->n4.child[i] == old) {
                n->n4.child[i] = new;
                return;
            }
        break;
    case NODE16:
        for (unsigned i = 0; i < n->children; i++)
            if (n->n16.keys[i] == c && n->n16.child[i] == old) {
                n->n16.child[i] = new;
                return;
            }
        break;
    case NODE48:
        if (n->n48.index[c] && n->n48.child[n->n48.index[c]-1] == old) {
            n->n48.child[n->n48.index[c]-1] = new;
            return;
        }
        break;
    case NODE256:
        if (n->n256.child[c] == old) {
            n->n256.child[c] = new;
            return;
        }
        break;
    default: break;
    }
}

static amap_node *grow(AMAP *a, amap_node *n, uint8_t c, amap_node *x) {
    if (n->type == NODE4 && n->children == 4) {
        amap_node *nn = alloc_node(a, NODE16);
        memcpy(nn->n16.keys, n->n4.keys, 4);
        memcpy(nn->n16.child, n->n4.child, 4*sizeof(void*));
        nn->children = 4;
        free_node(a, n);
        n = nn;
    }
    if (n->type == NODE16 && n->children == 16) {
        amap_node *nn = alloc_node(a, NODE48);
        memset(nn->n48.index, 0, 256);
        for (int i = 0; i < 16; i++) {
            nn->n48.index[n->n16.keys[i]] = i+1;
            nn->n48.child[i] = n->n16.child[i];
        }
        nn->children = 16;
        free_node(a, n);
        n = nn;
    }
    if (n->type == NODE48 && n->children == 48) {
        amap_node *nn = alloc_node(a, NODE256);
        memset(nn->n256.child, 0, 256*sizeof(void*));
        for (int i = 0; i < 256; i++)
            if (n->n48.index[i])
                nn->n256.child[i] = n->n48.child[n->n48.index[i]-1];
        nn->children = n->children;
        free_node(a, n);
        n = nn;
    }

    switch (n->type) {
    case NODE4:
        n->n4.keys[n->children] = c;
        n->n4.child[n->children++] = x;
        break;
    case NODE16:
        n->n16.keys[n->children] = c;
        n->n16.child[n->children++] = x;
        break;
    case NODE48:
        n->n48.index[c] = ++n->children;
        n->n48.child[n->children-1] = x;
        break;
    case NODE256:
        n->n256.child[c] = x;
        n->children++;
        break;
    default: break;
    }
    return n;
}

static amap_node *split_leaf(AMAP *a, amap_node *leaf, uint64_t new_key, void *new_val, int depth) {
    uint8_t old_bytes[8];
    memcpy(old_bytes, leaf->leaf.key, 8);
    uint8_t new_bytes[8];
    for (int i = 0; i < 8; i++) new_bytes[i] = key_byte(new_key, i);

    int diff = depth;
    while (diff < 8 && old_bytes[diff] == new_bytes[diff]) diff++;

    amap_node *branch = alloc_node(a, NODE4);
    branch->children = 0;
    amap_node *leaf_a = make_leaf(a, key_from_bytes(old_bytes), leaf->leaf.val);
    amap_node *leaf_b = make_leaf(a, new_key, new_val);
    branch = grow(a, branch, old_bytes[diff], leaf_a);
    branch = grow(a, branch, new_bytes[diff], leaf_b);

    if (diff == depth) {
        free_node(a, leaf);
        return branch;
    }

    amap_node *cur = branch;
    for (int d = diff - 1; d >= depth; d--) {
        amap_node *inner = alloc_node(a, NODE4);
        inner->children = 0;
        inner = grow(a, inner, old_bytes[d], cur);
        cur = inner;
    }
    free_node(a, leaf);
    return cur;
}

void amap_insert(AMAP *a, uint64_t k, void *v) {
    if (!a->root) {
        a->root = make_leaf(a, k, v);
        return;
    }

    amap_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    amap_node *cur = a->root;

    while (1) {
        path[depth] = cur;
        if (cur->type == LEAF) {
            uint64_t existing = key_from_bytes(cur->leaf.key);
            if (existing == k) {
                cur->leaf.val = v;
                return;
            }
            amap_node *nn = split_leaf(a, cur, k, v, depth);
            if (depth == 0) a->root = nn;
            else replace_child(path[depth-1], pbyte[depth-1], cur, nn);
            return;
        }
        uint8_t c = key_byte(k, depth);
        pbyte[depth] = c;
        amap_node *next = child(cur, c);
        if (!next) {
            amap_node *leaf = make_leaf(a, k, v);
            amap_node *new_cur = grow(a, cur, c, leaf);
            if (depth == 0) a->root = new_cur;
            else replace_child(path[depth-1], pbyte[depth-1], cur, new_cur);
            return;
        }
        cur = next;
        depth++;
    }
}

void *amap_search(AMAP *a, uint64_t k) {
    amap_node *cur = a->root;
    int depth = 0;
    while (cur) {
        if (cur->type == LEAF) {
            uint64_t existing = key_from_bytes(cur->leaf.key);
            return (existing == k) ? cur->leaf.val : NULL;
        }
        if (depth >= 8) return NULL;
        cur = child(cur, key_byte(k, depth));
        depth++;
    }
    return NULL;
}

// 从父节点中移除子节点条目（不修改 parent->children 计数，仅移动索引）
static void erase_child_entry(amap_node *parent, uint8_t byte, amap_node *child) {
    switch (parent->type) {
    case NODE4: {
        int pos = -1;
        for (unsigned i = 0; i < parent->children; i++) {
            if (parent->n4.keys[i] == byte && parent->n4.child[i] == child) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            for (unsigned i = pos; i < parent->children - 1; i++) {
                parent->n4.keys[i] = parent->n4.keys[i+1];
                parent->n4.child[i] = parent->n4.child[i+1];
            }
        }
        break;
    }
    case NODE16: {
        int pos = -1;
        for (unsigned i = 0; i < parent->children; i++) {
            if (parent->n16.keys[i] == byte && parent->n16.child[i] == child) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            for (unsigned i = pos; i < parent->children - 1; i++) {
                parent->n16.keys[i] = parent->n16.keys[i+1];
                parent->n16.child[i] = parent->n16.child[i+1];
            }
        }
        break;
    }
    case NODE48: {
        if (parent->n48.index[byte]) {
            int idx = parent->n48.index[byte] - 1;
            int last = parent->children - 1;
            if (idx != last) {
                parent->n48.child[idx] = parent->n48.child[last];
                for (int i = 0; i < 256; i++) {
                    if (parent->n48.index[i] == last + 1) {
                        parent->n48.index[i] = idx + 1;
                        break;
                    }
                }
            }
            parent->n48.index[byte] = 0;
        }
        break;
    }
    case NODE256: {
        if (parent->n256.child[byte] == child) {
            parent->n256.child[byte] = NULL;
        }
        break;
    }
    default: break;
    }
}

int amap_delete(AMAP *a, uint64_t k) {
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = key_byte(k, i);

    amap_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    amap_node *cur = a->root;

    // 查找叶子并记录路径
    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = key[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    if (!cur || cur->type != LEAF) return -1;
    if (key_from_bytes(cur->leaf.key) != k) return -1;

    // 释放叶子
    free_node(a, cur);

    // 如果叶子是根
    if (depth == 0) {
        a->root = NULL;
        return 0;
    }

    // 从父节点中移除叶子条目
    int i = depth - 1;
    amap_node *parent = path[i];
    uint8_t byte = pbyte[i];
    erase_child_entry(parent, byte, cur);   // cur 是已释放的叶子，但指针值仍可用于比较
    parent->children--;

    // 向上清理空节点
    while (i >= 0) {
        amap_node *node = path[i];
        if (node->children == 0) {
            if (i == 0) {
                free_node(a, node);
                a->root = NULL;
                return 0;
            } else {
                amap_node *grand = path[i-1];
                uint8_t gbyte = pbyte[i-1];
                erase_child_entry(grand, gbyte, node);
                grand->children--;
                free_node(a, node);
                i--;
                continue;
            }
        } else {
            break;
        }
    }
    return 0;
}

// 查找子树中最左叶子（最小键）
static amap_node *leftmost_leaf(amap_node *n) {
    if (!n) return NULL;
    while (n->type != LEAF) {
        switch (n->type) {
        case NODE4:
            if (n->children == 0) return NULL;
            n = n->n4.child[0];
            break;
        case NODE16:
            if (n->children == 0) return NULL;
            n = n->n16.child[0];
            break;
        case NODE48:
            for (int i = 0; i < 256; i++) {
                if (n->n48.index[i]) {
                    n = n->n48.child[n->n48.index[i]-1];
                    break;
                }
            }
            break;
        case NODE256:
            for (int i = 0; i < 256; i++) {
                if (n->n256.child[i]) {
                    n = n->n256.child[i];
                    break;
                }
            }
            break;
        default: return NULL;
        }
    }
    return n;
}

// 查找子树中最右叶子（最大键）
static amap_node *rightmost_leaf(amap_node *n) {
    if (!n) return NULL;
    while (n->type != LEAF) {
        switch (n->type) {
        case NODE4:
            if (n->children == 0) return NULL;
            n = n->n4.child[n->children-1];
            break;
        case NODE16:
            if (n->children == 0) return NULL;
            n = n->n16.child[n->children-1];
            break;
        case NODE48:
            for (int i = 255; i >= 0; i--) {
                if (n->n48.index[i]) {
                    n = n->n48.child[n->n48.index[i]-1];
                    break;
                }
            }
            break;
        case NODE256:
            for (int i = 255; i >= 0; i--) {
                if (n->n256.child[i]) {
                    n = n->n256.child[i];
                    break;
                }
            }
            break;
        default: return NULL;
        }
    }
    return n;
}

// 后继：大于 key 的最小键
uint64_t amap_next(AMAP *a, uint64_t key, void **out_val) {
    if (!a->root) return 0;

    uint8_t target[8];
    for (int i = 0; i < 8; i++) target[i] = key_byte(key, i);

    amap_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    amap_node *cur = a->root;

    // 沿着 key 向下，记录路径和路径上的字节
    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = target[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    // 情况1：当前节点是叶子且键等于 key
    if (cur && cur->type == LEAF && key_from_bytes(cur->leaf.key) == key) {
        // 从父节点向上回溯，找第一个可以转向更大字节的节点
        for (int i = depth - 1; i >= 0; i--) {
            amap_node *node = path[i];
            uint8_t byte = pbyte[i];
            uint8_t next_byte = 0xFF;
            switch (node->type) {
            case NODE4:
                for (unsigned j = 0; j < node->children; j++) {
                    if (node->n4.keys[j] > byte && node->n4.keys[j] < next_byte)
                        next_byte = node->n4.keys[j];
                }
                break;
            case NODE16:
                for (unsigned j = 0; j < node->children; j++) {
                    if (node->n16.keys[j] > byte && node->n16.keys[j] < next_byte)
                        next_byte = node->n16.keys[j];
                }
                break;
            case NODE48:
                for (int b = byte + 1; b < 256; b++) {
                    if (node->n48.index[b]) {
                        next_byte = b;
                        break;
                    }
                }
                break;
            case NODE256:
                for (int b = byte + 1; b < 256; b++) {
                    if (node->n256.child[b]) {
                        next_byte = b;
                        break;
                    }
                }
                break;
            default: break;
            }
            if (next_byte != 0xFF) {
                amap_node *next_node = child(node, next_byte);
                amap_node *leaf = leftmost_leaf(next_node);
                if (leaf) {
                    *out_val = leaf->leaf.val;
                    return key_from_bytes(leaf->leaf.key);
                }
            }
        }
        return 0;  // 没有更大的键
    }

    // 情况2：没有恰好等于 key 的叶子（或者 key 在树中不存在）
    // 从路径中找第一个可以转向更大字节的节点，然后取最左叶子
    for (int i = depth - 1; i >= 0; i--) {
        amap_node *node = path[i];
        uint8_t byte = pbyte[i];
        uint8_t next_byte = 0xFF;
        switch (node->type) {
        case NODE4:
            for (unsigned j = 0; j < node->children; j++) {
                if (node->n4.keys[j] > byte && node->n4.keys[j] < next_byte)
                    next_byte = node->n4.keys[j];
            }
            break;
        case NODE16:
            for (unsigned j = 0; j < node->children; j++) {
                if (node->n16.keys[j] > byte && node->n16.keys[j] < next_byte)
                    next_byte = node->n16.keys[j];
            }
            break;
        case NODE48:
            for (int b = byte + 1; b < 256; b++) {
                if (node->n48.index[b]) {
                    next_byte = b;
                    break;
                }
            }
            break;
        case NODE256:
            for (int b = byte + 1; b < 256; b++) {
                if (node->n256.child[b]) {
                    next_byte = b;
                    break;
                }
            }
            break;
        default: break;
        }
        if (next_byte != 0xFF) {
            amap_node *next_node = child(node, next_byte);
            amap_node *leaf = leftmost_leaf(next_node);
            if (leaf) {
                *out_val = leaf->leaf.val;
                return key_from_bytes(leaf->leaf.key);
            }
        }
    }

    // 情况3：整个树的最小键大于 key
    amap_node *min_leaf = leftmost_leaf(a->root);
    if (min_leaf && key_from_bytes(min_leaf->leaf.key) > key) {
        *out_val = min_leaf->leaf.val;
        return key_from_bytes(min_leaf->leaf.key);
    }
    return 0;
}

// 前驱：小于 key 的最大键
uint64_t amap_prev(AMAP *a, uint64_t key, void **out_val) {
    if (!a->root) return 0;

    uint8_t target[8];
    for (int i = 0; i < 8; i++) target[i] = key_byte(key, i);

    amap_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    amap_node *cur = a->root;

    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = target[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    if (cur && cur->type == LEAF && key_from_bytes(cur->leaf.key) == key) {
        // 等于 key，向上找小于当前字节的最大分支
        for (int i = depth - 1; i >= 0; i--) {
            amap_node *node = path[i];
            uint8_t byte = pbyte[i];
            uint8_t prev_byte = 0;
            switch (node->type) {
            case NODE4:
                for (unsigned j = 0; j < node->children; j++) {
                    if (node->n4.keys[j] < byte && node->n4.keys[j] > prev_byte)
                        prev_byte = node->n4.keys[j];
                }
                break;
            case NODE16:
                for (unsigned j = 0; j < node->children; j++) {
                    if (node->n16.keys[j] < byte && node->n16.keys[j] > prev_byte)
                        prev_byte = node->n16.keys[j];
                }
                break;
            case NODE48:
                for (int b = byte - 1; b >= 0; b--) {
                    if (node->n48.index[b]) {
                        prev_byte = b;
                        break;
                    }
                }
                break;
            case NODE256:
                for (int b = byte - 1; b >= 0; b--) {
                    if (node->n256.child[b]) {
                        prev_byte = b;
                        break;
                    }
                }
                break;
            default: break;
            }
            if (prev_byte != 0) {
                amap_node *prev_node = child(node, prev_byte);
                amap_node *leaf = rightmost_leaf(prev_node);
                if (leaf) {
                    *out_val = leaf->leaf.val;
                    return key_from_bytes(leaf->leaf.key);
                }
            }
        }
        return 0;
    }

    // 没有恰好相等的叶子，从路径中找第一个可转向更小字节的节点
    for (int i = depth - 1; i >= 0; i--) {
        amap_node *node = path[i];
        uint8_t byte = pbyte[i];
        uint8_t prev_byte = 0;
        switch (node->type) {
        case NODE4:
            for (unsigned j = 0; j < node->children; j++) {
                if (node->n4.keys[j] < byte && node->n4.keys[j] > prev_byte)
                    prev_byte = node->n4.keys[j];
            }
            break;
        case NODE16:
            for (unsigned j = 0; j < node->children; j++) {
                if (node->n16.keys[j] < byte && node->n16.keys[j] > prev_byte)
                    prev_byte = node->n16.keys[j];
            }
            break;
        case NODE48:
            for (int b = byte - 1; b >= 0; b--) {
                if (node->n48.index[b]) {
                    prev_byte = b;
                    break;
                }
            }
            break;
        case NODE256:
            for (int b = byte - 1; b >= 0; b--) {
                if (node->n256.child[b]) {
                    prev_byte = b;
                    break;
                }
            }
            break;
        default: break;
        }
        if (prev_byte != 0) {
            amap_node *prev_node = child(node, prev_byte);
            amap_node *leaf = rightmost_leaf(prev_node);
            if (leaf) {
                *out_val = leaf->leaf.val;
                return key_from_bytes(leaf->leaf.key);
            }
        }
    }

    // 整个树的最大键小于 key
    amap_node *max_leaf = rightmost_leaf(a->root);
    if (max_leaf && key_from_bytes(max_leaf->leaf.key) < key) {
        *out_val = max_leaf->leaf.val;
        return key_from_bytes(max_leaf->leaf.key);
    }
    return 0;
}

// Insert a (byte, child) pair into a node that already has enough capacity.
static void add_child_to_node(amap_node *n, uint8_t byte, amap_node *child) {
    switch (n->type) {
        case NODE4:
            n->n4.keys[n->children] = byte;
            n->n4.child[n->children++] = child;
            break;
        case NODE16:
            n->n16.keys[n->children] = byte;
            n->n16.child[n->children++] = child;
            break;
        case NODE48:
            n->n48.index[byte] = n->children + 1;
            n->n48.child[n->children++] = child;
            break;
        case NODE256:
            n->n256.child[byte] = child;
            n->children++;
            break;
        default: break;
    }
}

// Recursively shrink a subtree. Returns the (possibly replaced) node.
static amap_node *shrink_node(AMAP *a, amap_node *n,
                              amap_node *parent, uint8_t parent_byte) {
    if (!n || n->type == LEAF) return n;

    // 1. Recursively shrink all children
    switch (n->type) {
        case NODE4:
            for (unsigned i = 0; i < n->children; i++) {
                amap_node *child = n->n4.child[i];
                amap_node *new_child = shrink_node(a, child, n, n->n4.keys[i]);
                if (new_child != child) n->n4.child[i] = new_child;
            }
            break;
        case NODE16:
            for (unsigned i = 0; i < n->children; i++) {
                amap_node *child = n->n16.child[i];
                amap_node *new_child = shrink_node(a, child, n, n->n16.keys[i]);
                if (new_child != child) n->n16.child[i] = new_child;
            }
            break;
        case NODE48:
            for (unsigned i = 0; i < n->children; i++) {
                // find byte that maps to index i
                for (int b = 0; b < 256; b++) {
                    if (n->n48.index[b] == i+1) {
                        amap_node *child = n->n48.child[i];
                        amap_node *new_child = shrink_node(a, child, n, (uint8_t)b);
                        if (new_child != child) n->n48.child[i] = new_child;
                        break;
                    }
                }
            }
            break;
        case NODE256:
            for (int b = 0; b < 256; b++) {
                amap_node *child = n->n256.child[b];
                if (child) {
                    amap_node *new_child = shrink_node(a, child, n, (uint8_t)b);
                    if (new_child != child) n->n256.child[b] = new_child;
                }
            }
            break;
        default: break;
    }

    uint32_t cnt = n->children;
    if (cnt == 0) {
        // Should not happen (deletion already removes empty nodes), but handle safely.
        if (parent) erase_child_entry(parent, parent_byte, n);
        free_node(a, n);
        return NULL;
    }

    // 2. Determine the smallest node type that can hold `cnt` children
    AMAP_NODE old_type = n->type;
    AMAP_NODE new_type = old_type;
    if (cnt <= 4) new_type = NODE4;
    else if (cnt <= 16) new_type = NODE16;
    else if (cnt <= 48) new_type = NODE48;
    else new_type = NODE256;

    if (new_type == old_type) return n;   // no shrink needed

    // 3. Create a new, smaller node and copy all children
    amap_node *new_node = alloc_node(a, new_type);
    if (!new_node) return n;   // allocation failed – keep the old node

    // Copy children (order does not matter for correctness)
    switch (old_type) {
        case NODE4:
            for (unsigned i = 0; i < cnt; i++)
                add_child_to_node(new_node, n->n4.keys[i], n->n4.child[i]);
            break;
        case NODE16:
            for (unsigned i = 0; i < cnt; i++)
                add_child_to_node(new_node, n->n16.keys[i], n->n16.child[i]);
            break;
        case NODE48:
            for (int b = 0; b < 256; b++) {
                if (n->n48.index[b]) {
                    int idx = n->n48.index[b] - 1;
                    add_child_to_node(new_node, (uint8_t)b, n->n48.child[idx]);
                }
            }
            break;
        case NODE256:
            for (int b = 0; b < 256; b++) {
                if (n->n256.child[b])
                    add_child_to_node(new_node, (uint8_t)b, n->n256.child[b]);
            }
            break;
        default: break;
    }

    // 4. Update parent (or root) and free the old node
    if (parent) {
        replace_child(parent, parent_byte, n, new_node);
    } else {
        a->root = new_node;
    }
    free_node(a, n);
    return new_node;
}

// Public API: compress the entire tree by shrinking oversized internal nodes.
int amap_shrink(AMAP *a) {
    if (!a) return -1;
    if (a->root && a->root->type != LEAF) {
        a->root = shrink_node(a, a->root, NULL, 0);
    }
    return 0;
}

int amap_delete_with_shrink(AMAP *a, uint64_t k) {
    if (!a || !a->root) return -1;

    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = key_byte(k, i);

    amap_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    amap_node *cur = a->root;

    // 查找叶子并记录路径
    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = key[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    if (!cur || cur->type != LEAF) return -1;
    if (key_from_bytes(cur->leaf.key) != k) return -1;

    // 释放叶子
    free_node(a, cur);

    // 如果叶子是根
    if (depth == 0) {
        a->root = NULL;
        return 0;
    }

    // 从父节点中移除叶子条目
    int i = depth - 1;
    amap_node *parent = path[i];
    uint8_t byte = pbyte[i];
    erase_child_entry(parent, byte, cur);
    parent->children--;

    // 自底向上处理路径上的所有节点：删除空节点 + 收缩非空节点
    while (i >= 0) {
        amap_node *node = path[i];
        uint32_t cnt = node->children;

        if (cnt == 0) {
            // 空节点：删除并继续向上
            if (i == 0) {
                free_node(a, node);
                a->root = NULL;
                return 0;
            } else {
                amap_node *grand = path[i-1];
                uint8_t gbyte = pbyte[i-1];
                erase_child_entry(grand, gbyte, node);
                grand->children--;
                free_node(a, node);
                i--;
                continue;
            }
        } else {
            // 非空节点：尝试收缩到更小的节点类型
            AMAP_NODE old_type = node->type;
            AMAP_NODE new_type = old_type;
            if (cnt <= 4) new_type = NODE4;
            else if (cnt <= 16) new_type = NODE16;
            else if (cnt <= 48) new_type = NODE48;
            else new_type = NODE256;

            if (new_type != old_type) {
                // 创建新的小节点
                amap_node *new_node = alloc_node(a, new_type);
                if (!new_node) return -1; // 分配失败，保持原结构不变

                // 复制所有孩子
                switch (old_type) {
                    case NODE4:
                        for (unsigned j = 0; j < cnt; j++)
                            add_child_to_node(new_node, node->n4.keys[j], node->n4.child[j]);
                        break;
                    case NODE16:
                        for (unsigned j = 0; j < cnt; j++)
                            add_child_to_node(new_node, node->n16.keys[j], node->n16.child[j]);
                        break;
                    case NODE48:
                        for (int b = 0; b < 256; b++) {
                            if (node->n48.index[b]) {
                                int idx = node->n48.index[b] - 1;
                                add_child_to_node(new_node, (uint8_t)b, node->n48.child[idx]);
                            }
                        }
                        break;
                    case NODE256:
                        for (int b = 0; b < 256; b++) {
                            if (node->n256.child[b])
                                add_child_to_node(new_node, (uint8_t)b, node->n256.child[b]);
                        }
                        break;
                    default: break;
                }

                // 更新父节点或根
                if (i == 0) {
                    a->root = new_node;
                } else {
                    replace_child(path[i-1], pbyte[i-1], node, new_node);
                }
                free_node(a, node);
                // 新节点已经替换了原节点，继续向上处理父节点（父节点的 children 数未变，但可能之前已经减过1）
                // 父节点会在下一轮循环中被处理（如果它需要收缩）
            }
        }
        i--; // 继续向上
    }
    return 0;
}

// 递归释放子树
static void free_subtree(AMAP *a, amap_node *n) {
    if (!n) return;
    if (n->type == LEAF) {
        free_node(a, n);
        return;
    }
    // 释放所有子节点
    switch (n->type) {
        case NODE4:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree(a, n->n4.child[i]);
            break;
        case NODE16:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree(a, n->n16.child[i]);
            break;
        case NODE48:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree(a, n->n48.child[i]);
            break;
        case NODE256:
            for (int i = 0; i < 256; i++)
                if (n->n256.child[i])
                    free_subtree(a, n->n256.child[i]);
            break;
        default: break;
    }
    free_node(a, n);
}

void amap_destroy(AMAP *a) {
    if (!a) return;
    free_subtree(a, a->root);
    a->root = NULL;
    // 注意：不释放 a 本身
}

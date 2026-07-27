
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "artset.h"

typedef enum {
    LEAF,
    NODE4,
    NODE16,
    NODE48,
    NODE256
} ASET_NODE;

struct aset_node {
    union {
        struct { uint8_t keys[4]; struct aset_node *child[4]; } n4;
        struct { uint8_t keys[16]; struct aset_node *child[16]; } n16;
        struct { uint8_t index[256]; struct aset_node *child[48]; } n48;
        struct { struct aset_node *child[256]; } n256;
        struct { uint8_t key[ASET_KLEN]; } leaf;
    };
    uint32_t children;
    ASET_NODE type;
};

static aset_conf default_conf = (aset_conf) {
    .malloc = malloc,
    .calloc = calloc,
    .free   = free
}; 

ASET* aset_create(aset_conf *c) {
    if (!c) c = &default_conf;
    if (!c->malloc) c->malloc = malloc;
    if (!c->calloc) c->calloc = calloc;
    if (!c->free) c->free = free;
    ASET *tree = (ASET*)c->malloc(sizeof(ASET));
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

static aset_node *alloc_node(ASET *a, ASET_NODE t) {
    aset_node *n = a->conf->calloc(1, sizeof(aset_node));
    n->type = t;
    return n;
}

static void free_node(ASET *a, aset_node *n) {
    if (n) a->conf->free(n);
}

static aset_node *make_leaf(ASET *a, uint64_t k) {
    aset_node *n = alloc_node(a, LEAF);
    for (int i = 0; i < 8; i++) n->leaf.key[i] = key_byte(k, i);
    return n;
}

static aset_node *child(aset_node *n, uint8_t c) {
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

static void replace_child(aset_node *n, uint8_t c, aset_node *old, aset_node *new) {
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

static aset_node *grow(ASET *a, aset_node *n, uint8_t c, aset_node *x) {
    if (n->type == NODE4 && n->children == 4) {
        aset_node *nn = alloc_node(a, NODE16);
        memcpy(nn->n16.keys, n->n4.keys, 4);
        memcpy(nn->n16.child, n->n4.child, 4*sizeof(void*));
        nn->children = 4;
        free_node(a, n);
        n = nn;
    }
    if (n->type == NODE16 && n->children == 16) {
        aset_node *nn = alloc_node(a, NODE48);
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
        aset_node *nn = alloc_node(a, NODE256);
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

static aset_node *split_leaf(ASET *a, aset_node *leaf, uint64_t new_key, int depth) {
    uint8_t old_bytes[8];
    memcpy(old_bytes, leaf->leaf.key, 8);
    uint8_t new_bytes[8];
    for (int i = 0; i < 8; i++) new_bytes[i] = key_byte(new_key, i);

    int diff = depth;
    while (diff < 8 && old_bytes[diff] == new_bytes[diff]) diff++;

    aset_node *branch = alloc_node(a, NODE4);
    branch->children = 0;
    aset_node *leaf_a = make_leaf(a, key_from_bytes(old_bytes));
    aset_node *leaf_b = make_leaf(a, new_key);
    branch = grow(a, branch, old_bytes[diff], leaf_a);
    branch = grow(a, branch, new_bytes[diff], leaf_b);

    if (diff == depth) {
        free_node(a, leaf);
        return branch;
    }

    aset_node *cur = branch;
    for (int d = diff - 1; d >= depth; d--) {
        aset_node *inner = alloc_node(a, NODE4);
        inner->children = 0;
        inner = grow(a, inner, old_bytes[d], cur);
        cur = inner;
    }
    free_node(a, leaf);
    return cur;
}

void aset_insert(ASET *a, uint64_t k) {
    if (k == 0) return;  // 0 视为无效，不插入
    if (!a->root) {
        a->root = make_leaf(a, k);
        return;
    }

    aset_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    aset_node *cur = a->root;

    while (1) {
        path[depth] = cur;
        if (cur->type == LEAF) {
            uint64_t existing = key_from_bytes(cur->leaf.key);
            if (existing == k) return;  // 已存在，直接返回
            aset_node *nn = split_leaf(a, cur, k, depth);
            if (depth == 0) a->root = nn;
            else replace_child(path[depth-1], pbyte[depth-1], cur, nn);
            return;
        }
        uint8_t c = key_byte(k, depth);
        pbyte[depth] = c;
        aset_node *next = child(cur, c);
        if (!next) {
            aset_node *leaf = make_leaf(a, k);
            aset_node *new_cur = grow(a, cur, c, leaf);
            if (depth == 0) a->root = new_cur;
            else replace_child(path[depth-1], pbyte[depth-1], cur, new_cur);
            return;
        }
        cur = next;
        depth++;
    }
}

int aset_contains(ASET *a, uint64_t k) {
    if (k == 0) return 0;
    aset_node *cur = a->root;
    int depth = 0;
    while (cur) {
        if (cur->type == LEAF) {
            uint64_t existing = key_from_bytes(cur->leaf.key);
            return (existing == k) ? 1 : 0;
        }
        if (depth >= 8) return 0;
        cur = child(cur, key_byte(k, depth));
        depth++;
    }
    return 0;
}

static void erase_child_entry(aset_node *parent, uint8_t byte, aset_node *child) {
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

int aset_delete(ASET *a, uint64_t k) {
    if (k == 0) return -1;
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = key_byte(k, i);

    aset_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    aset_node *cur = a->root;

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

    free_node(a, cur);

    if (depth == 0) {
        a->root = NULL;
        return 0;
    }

    int i = depth - 1;
    aset_node *parent = path[i];
    uint8_t byte = pbyte[i];
    erase_child_entry(parent, byte, cur);
    parent->children--;

    while (i >= 0) {
        aset_node *node = path[i];
        if (node->children == 0) {
            if (i == 0) {
                free_node(a, node);
                a->root = NULL;
                return 0;
            } else {
                aset_node *grand = path[i-1];
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

static aset_node *leftmost_leaf(aset_node *n) {
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

static aset_node *rightmost_leaf(aset_node *n) {
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

uint64_t aset_next(ASET *a, uint64_t key) {
    if (!a->root) return 0;

    uint8_t target[8];
    for (int i = 0; i < 8; i++) target[i] = key_byte(key, i);

    aset_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    aset_node *cur = a->root;

    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = target[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    if (cur && cur->type == LEAF && key_from_bytes(cur->leaf.key) == key) {
        for (int i = depth - 1; i >= 0; i--) {
            aset_node *node = path[i];
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
                aset_node *next_node = child(node, next_byte);
                aset_node *leaf = leftmost_leaf(next_node);
                if (leaf) return key_from_bytes(leaf->leaf.key);
            }
        }
        return 0;
    }

    for (int i = depth - 1; i >= 0; i--) {
        aset_node *node = path[i];
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
            aset_node *next_node = child(node, next_byte);
            aset_node *leaf = leftmost_leaf(next_node);
            if (leaf) return key_from_bytes(leaf->leaf.key);
        }
    }

    aset_node *min_leaf = leftmost_leaf(a->root);
    if (min_leaf && key_from_bytes(min_leaf->leaf.key) > key) {
        return key_from_bytes(min_leaf->leaf.key);
    }
    return 0;
}

uint64_t aset_prev(ASET *a, uint64_t key) {
    if (!a->root) return 0;

    uint8_t target[8];
    for (int i = 0; i < 8; i++) target[i] = key_byte(key, i);

    aset_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    aset_node *cur = a->root;

    while (cur && depth < 8) {
        path[depth] = cur;
        if (cur->type == LEAF) break;
        uint8_t c = target[depth];
        pbyte[depth] = c;
        cur = child(cur, c);
        depth++;
    }

    if (cur && cur->type == LEAF && key_from_bytes(cur->leaf.key) == key) {
        for (int i = depth - 1; i >= 0; i--) {
            aset_node *node = path[i];
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
                aset_node *prev_node = child(node, prev_byte);
                aset_node *leaf = rightmost_leaf(prev_node);
                if (leaf) return key_from_bytes(leaf->leaf.key);
            }
        }
        return 0;
    }

    for (int i = depth - 1; i >= 0; i--) {
        aset_node *node = path[i];
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
            aset_node *prev_node = child(node, prev_byte);
            aset_node *leaf = rightmost_leaf(prev_node);
            if (leaf) return key_from_bytes(leaf->leaf.key);
        }
    }

    aset_node *max_leaf = rightmost_leaf(a->root);
    if (max_leaf && key_from_bytes(max_leaf->leaf.key) < key) {
        return key_from_bytes(max_leaf->leaf.key);
    }
    return 0;
}

// ========== 辅助：向节点中添加子节点（假设空间足够） ==========
static void add_child_to_node(aset_node *n, uint8_t byte, aset_node *child) {
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

// ========== 递归压缩子树 ==========
static aset_node *shrink_node(ASET *a, aset_node *n,
                              aset_node *parent, uint8_t parent_byte) {
    if (!n || n->type == LEAF) return n;

    // 1. 递归压缩所有子节点
    switch (n->type) {
        case NODE4:
            for (unsigned i = 0; i < n->children; i++) {
                aset_node *child = n->n4.child[i];
                aset_node *new_child = shrink_node(a, child, n, n->n4.keys[i]);
                if (new_child != child) n->n4.child[i] = new_child;
            }
            break;
        case NODE16:
            for (unsigned i = 0; i < n->children; i++) {
                aset_node *child = n->n16.child[i];
                aset_node *new_child = shrink_node(a, child, n, n->n16.keys[i]);
                if (new_child != child) n->n16.child[i] = new_child;
            }
            break;
        case NODE48:
            for (unsigned i = 0; i < n->children; i++) {
                // 找到第 i 个子节点对应的字节
                for (int b = 0; b < 256; b++) {
                    if (n->n48.index[b] == i+1) {
                        aset_node *child = n->n48.child[i];
                        aset_node *new_child = shrink_node(a, child, n, (uint8_t)b);
                        if (new_child != child) n->n48.child[i] = new_child;
                        break;
                    }
                }
            }
            break;
        case NODE256:
            for (int b = 0; b < 256; b++) {
                aset_node *child = n->n256.child[b];
                if (child) {
                    aset_node *new_child = shrink_node(a, child, n, (uint8_t)b);
                    if (new_child != child) n->n256.child[b] = new_child;
                }
            }
            break;
        default: break;
    }

    uint32_t cnt = n->children;
    if (cnt == 0) {
        // 空节点（理论上删除时已处理，但为安全仍清理）
        if (parent) erase_child_entry(parent, parent_byte, n);
        free_node(a, n);
        return NULL;
    }

    // 2. 确定可以降级的最小节点类型
    ASET_NODE old_type = n->type;
    ASET_NODE new_type = old_type;
    if (cnt <= 4) new_type = NODE4;
    else if (cnt <= 16) new_type = NODE16;
    else if (cnt <= 48) new_type = NODE48;
    else new_type = NODE256;

    if (new_type == old_type) return n;   // 无需压缩

    // 3. 创建新节点并复制所有子节点
    aset_node *new_node = alloc_node(a, new_type);
    if (!new_node) return n;   // 分配失败，保持原结构

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

    // 4. 更新父节点或根，释放旧节点
    if (parent) {
        replace_child(parent, parent_byte, n, new_node);
    } else {
        a->root = new_node;
    }
    free_node(a, n);
    return new_node;
}

// ========== 公开 API：全树压缩 ==========
int aset_shrink(ASET *a) {
    if (!a) return -1;
    if (a->root && a->root->type != LEAF) {
        a->root = shrink_node(a, a->root, NULL, 0);
    }
    return 0;
}

// ========== 删除并沿路径压缩（自底向上） ==========
int aset_delete_with_shrink(ASET *a, uint64_t k) {
    if (!a || !a->root || k == 0) return -1;

    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = key_byte(k, i);

    aset_node *path[9];
    uint8_t pbyte[9];
    int depth = 0;
    aset_node *cur = a->root;

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
    aset_node *parent = path[i];
    uint8_t byte = pbyte[i];
    erase_child_entry(parent, byte, cur);
    parent->children--;

    // 自底向上处理路径上的所有节点
    while (i >= 0) {
        aset_node *node = path[i];
        uint32_t cnt = node->children;

        if (cnt == 0) {
            // 空节点：删除并继续向上
            if (i == 0) {
                free_node(a, node);
                a->root = NULL;
                return 0;
            } else {
                aset_node *grand = path[i-1];
                uint8_t gbyte = pbyte[i-1];
                erase_child_entry(grand, gbyte, node);
                grand->children--;
                free_node(a, node);
                i--;
                continue;
            }
        } else {
            // 非空节点：尝试降级
            ASET_NODE old_type = node->type;
            ASET_NODE new_type = old_type;
            if (cnt <= 4) new_type = NODE4;
            else if (cnt <= 16) new_type = NODE16;
            else if (cnt <= 48) new_type = NODE48;
            else new_type = NODE256;

            if (new_type != old_type) {
                aset_node *new_node = alloc_node(a, new_type);
                if (!new_node) return -1; // 降级失败，保持原样

                // 复制子节点
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
                // 新节点已替换原节点，继续向上（父节点的 children 数未变，但上一轮可能已减）
            }
        }
        i--; // 继续向上处理父节点
    }
    return 0;
}

// 递归释放子树（set 版本，无 val）
static void free_subtree_set(ASET *a, aset_node *n) {
    if (!n) return;
    if (n->type == LEAF) {
        free_node(a, n);
        return;
    }
    switch (n->type) {
        case NODE4:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree_set(a, n->n4.child[i]);
            break;
        case NODE16:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree_set(a, n->n16.child[i]);
            break;
        case NODE48:
            for (unsigned i = 0; i < n->children; i++)
                free_subtree_set(a, n->n48.child[i]);
            break;
        case NODE256:
            for (int i = 0; i < 256; i++)
                if (n->n256.child[i])
                    free_subtree_set(a, n->n256.child[i]);
            break;
        default: break;
    }
    free_node(a, n);
}

void aset_destroy(ASET *a) {
    if (!a) return;
    free_subtree_set(a, a->root);
    a->root = NULL;
    // 不释放 a 本身
}

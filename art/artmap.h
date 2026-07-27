
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AMAP_KLEN 8

typedef struct amap_conf {
    void* (*malloc)(size_t size);
    void* (*calloc)(size_t nmemb, size_t size);
    void  (*free)(void *ptr);
} amap_conf;

typedef struct amap_node amap_node;

typedef struct {
    amap_node *root;
    amap_conf *conf;
} AMAP;

AMAP* amap_create(amap_conf *conf);

void amap_insert(AMAP *a, uint64_t k, void *v);

void *amap_search(AMAP *a, uint64_t k);

int amap_delete(AMAP *a, uint64_t k);

uint64_t amap_next(AMAP *a, uint64_t k, void **out_val);
uint64_t amap_prev(AMAP *a, uint64_t k, void **out_val);

int amap_shrink(AMAP *a);
int amap_delete_with_shrink(AMAP *a, uint64_t k);
void amap_destroy(AMAP *a);

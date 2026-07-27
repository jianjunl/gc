
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ASET_KLEN 8

typedef struct aset_conf {
    void* (*malloc)(size_t size);
    void* (*calloc)(size_t nmemb, size_t size);
    void  (*free)(void *ptr);
} aset_conf;

typedef struct aset_node aset_node;

typedef struct {
    aset_node *root;
    aset_conf *conf;
} ASET;

ASET* aset_create(aset_conf *conf);

void aset_insert(ASET *a, uint64_t k);
int aset_contains(ASET *a, uint64_t k);
int aset_delete(ASET *a, uint64_t k);
uint64_t aset_next(ASET *a, uint64_t key);
uint64_t aset_prev(ASET *a, uint64_t key);

int aset_shrink(ASET *a);
int aset_delete_with_shrink(ASET *a, uint64_t k);
void aset_destroy(ASET *a);

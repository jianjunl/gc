/* ============================================================
 * gc.block.c - 
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

gc_block_t *gc_blocks = NULL;

gc_block_t* gc_find_block(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    for (gc_block_t *blk = gc_blocks; blk; blk = blk->next) {
        uintptr_t start = (uintptr_t)blk->ptr;
        uintptr_t end   = start + blk->size;
        if (p >= start && p < end) return blk;
    }
    return NULL;
}

/* ============================================================
 * gc.init.c - Initialization (multi-thread & single-thread)
 *
 * Implements  get stack top and bottom addresses
 *             initialize the collector.
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

void *gc_stack_bottom = NULL;

// ---------- Stack bottom detection (cross‑platform skeleton) ---------- //
static void* get_stack_bottom(void) {
#if defined(__linux__)
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return NULL;
    char line[256];
    void *bottom = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "[stack]")) {
            unsigned long start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2)
                bottom = (void*)end;
            break;
        }
    }
    fclose(fp);
    LOG_DEBUG("main thread stack bottom = %p", bottom);
    return bottom;
#elif defined(__APPLE__) && defined(__MACH__)
    return pthread_get_stackaddr_np(pthread_self());
#elif defined(_WIN32)
    #pragma message("warning: Windows stack detection is a stub")
    return NULL;
#else
    #error "unsupported platform"
    return NULL;
#endif
}

/*
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
void* get_stack_top(void) {
    int dummy;
    return (void*)&dummy;
}
#pragma GCC diagnostic pop
*/

void* get_stack_top() {
    pthread_t self = pthread_self();
    pthread_attr_t attr;
    void *addr;
    size_t size;

    pthread_getattr_np(self, &attr);
    pthread_attr_getstack(&attr, &addr, &size);

    // addr is the lowest address, addr + size is the "top" (base) 
    // because stacks usually grow downwards.
    //printf("Stack base: %p\n", addr);
    //printf("Stack size: %zu bytes\n", size);
    //printf("Stack top boundary: %p\n", (char*)addr + size);

    pthread_attr_destroy(&attr);
    gc_stack_bottom = addr;
    return (void *)((char*)addr + size);
}

#if GC_MULTITHREAD 

// ---------- STW initialisation ---------- //
extern void gc_stw_init(void);

#endif // GC_MULTITHREAD

bool gc_initialized = false;

// ---------- Common initialisation ---------- //
void gc_init(void) {
    if (gc_initialized) return;
#if GC_MULTITHREAD
    gc_stw_init();
#endif // GC_MULTITHREAD
    gc_stack_bottom = get_stack_bottom();
    if (!gc_stack_bottom) {
        int dummy;
        gc_stack_bottom = (void*)((uintptr_t)&dummy + 4096);
        LOG_DEBUG("fallback main thread stack bottom = %p", gc_stack_bottom);
    }
#if GC_MULTITHREAD
    gc_register_thread_stack(gc_stack_bottom);
#endif // GC_MULTITHREAD
    gc_add_root(&gc_current_exception);
    gc_initialized = true;
    LOG_DEBUG("GC initialised, threshold = %zu bytes", gc_threshold);
}

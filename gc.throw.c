/* ============================================================
 * gc.throw.c – Simple try/throw/catch exception mechanism
 *
 * Uses sigsetjmp/siglongjmp. Each thread has its own
 * exception context. gc_throw() locks briefly while setting
 * the exception code, then unlocks before jumping so that
 * the lock is never held across a non‑local transfer.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include "gc.h"
#define DEBUG // Uncomment to debug
#include "log.h"

// Thread‑local pointer to the current exception frame
_Thread_local gc_exception_t *gc_current_exception = NULL;

// Global lock for protecting exception code writes (paranoid, but works)
static gc_mutex_t *gc_exception_lock = &GC_PTHREAD_MUTEX_INITIALIZER;

/* ---------- gc_throw ---------- */
void gc_throw(int code) {
    if (!gc_current_exception) {
        LOG_FATAL("throw without a TRY block\n");
    }

    // Set the exception code under a brief lock
    gc_pthread_mutex_lock(gc_exception_lock);
    gc_current_exception->code = code;
    gc_pthread_mutex_unlock(gc_exception_lock);

    // Jump back to the matching TRY
    siglongjmp(gc_current_exception->buf, 1);
}

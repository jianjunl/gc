/* ============================================================
 * gc.stw.c - Stop‑The‑World synchronisation
 *
 * Uses POSIX signals (SIGUSR1/SIGUSR2) and semaphores to suspend
 * and resume all registered threads during a collection.  Only
 * compiled when GC_MULTITHREAD == 1.
 *
 * Author: jianjunliu@126.com Generated with DeepSeek assistance
 * License: MIT
 * ============================================================ */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <semaphore.h>
#include <ucontext.h>
#include <errno.h>
#include "gc.h"
//#define DEBUG // Uncomment to debug
#include "log.h"

#if GC_MULTITHREAD 

/* ---------- Thread information ---------- */
typedef struct gc_thread_info {
    pthread_t           tid;
    void               *stack_bottom;
    void               *stack_top;
    ucontext_t          suspend_ctx;
    bool                ctx_valid;
    struct gc_thread_info *next;
} gc_thread_info_t;

extern gc_thread_info_t *gc_threads;
extern pthread_mutex_t   gc_threads_lock;

// ---------- Stop‑The‑World synchronisation ---------- //
static volatile bool gc_stw_in_progress = false;
       volatile int  gc_stw_suspended_count = 0;
static sem_t         gc_stw_sem_ack;
static sem_t         gc_stw_sem_resume;
_Thread_local        gc_thread_info_t *gc_tls_self_info = NULL;

void gc_update_stack(gc_thread_info_t *t) {
    pthread_attr_t attr;
    void *addr;
    size_t size;

    pthread_getattr_np(t->tid, &attr);
    pthread_attr_getstack(&attr, &addr, &size);

    t->stack_top = (void *)((char*)addr + size);
    t->stack_bottom = addr;
    pthread_attr_destroy(&attr);
}

// ---------- Signal handlers ---------- //
static void gc_stw_resume_handler(int sig) { (void)sig; }
static void gc_stw_suspend_handler(int sig) {
    (void)sig;
    ucontext_t ctx;
    getcontext(&ctx);
    int dummy;
    void *stack_top = &dummy;
    if (gc_tls_self_info) {
        gc_tls_self_info->stack_top = stack_top;
        gc_tls_self_info->suspend_ctx = ctx;
        gc_tls_self_info->ctx_valid = true;
        gc_update_stack(gc_tls_self_info);
    }
    sem_post(&gc_stw_sem_ack);
    while (sem_wait(&gc_stw_sem_resume) == -1 && errno == EINTR);
}

// ---------- STW initialisation ---------- //
void gc_stw_init(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;
    struct sigaction sa = { .sa_flags = 0 };
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = gc_stw_suspend_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = gc_stw_resume_handler;
    sigaction(SIGUSR2, &sa, NULL);
    sem_init(&gc_stw_sem_ack, 0, 0);
    sem_init(&gc_stw_sem_resume, 0, 0);
    gc_add_root(&gc_tls_self_info);
    LOG_DEBUG("STW signal handlers installed");
}

// ---------- STW helpers ---------- //
void gc_stop_all_threads(void) {
    LOG_DEBUG("stopping all threads");
    gc_stw_in_progress = true;
    gc_stw_suspended_count = 0;
    pthread_mutex_lock(&gc_threads_lock);
    for (gc_thread_info_t *t = gc_threads; t; t = t->next) {
        if (!pthread_equal(t->tid, pthread_self())) {
            pthread_kill(t->tid, SIGUSR1);
            gc_stw_suspended_count++;
        }
    }
    pthread_mutex_unlock(&gc_threads_lock);
    for (int i = 0; i < gc_stw_suspended_count; i++)
        while (sem_wait(&gc_stw_sem_ack) == -1 && errno == EINTR);
    LOG_DEBUG("suspended %d threads", gc_stw_suspended_count);
}

void gc_resume_all_threads(void) {
    LOG_DEBUG("resuming %d threads", gc_stw_suspended_count);
    for (int i = 0; i < gc_stw_suspended_count; i++)
        sem_post(&gc_stw_sem_resume);
    gc_stw_in_progress = false;
    gc_stw_suspended_count = 0;
}

#endif // GC_MULTITHREAD

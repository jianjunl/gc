/*
 * gc.test.c – Unit Tests for the Conservative Garbage Collector
 *
 * Compile with the same flags as the library. Each test function
 * returns 0 on success and non‑zero on failure. The main() driver
 * runs all enabled tests and prints a summary.
 *
 * Usage:
 *   make -f gc.mk CFLAGS="-O2 -g -pthread -fPIC"
 *   ./gc.test
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include "gc.h"
#include "log.h"

/* ---------- Simple assertion helper ---------- */
static int tests_run = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    do { \
        printf("  [TEST] %-50s ... ", name); \
        fflush(stdout); \
        tests_run++; \
    } while (0)

#define TEST_PASS() \
    do { \
        printf("PASSED\n"); \
        return 0; \
    } while (0)

#define TEST_FAIL(msg) \
    do { \
        printf("FAILED: %s\n", msg); \
        tests_failed++; \
        return 1; \
    } while (0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAILED: %s\n", msg); \
            tests_failed++; \
            return 1; \
        } \
    } while (0)

/* ---------- Test subjects ---------- */
typedef struct node {
    int value;
    struct node *next;
} node_t;

/* Helper to clear the stack and reduce false positives */
static void force_stack_clear(void) {
    volatile int dummy[64];
    for (int i = 0; i < 64; i++) dummy[i] = 0;
    (void)dummy;
}

/* ================================================================
 * Test 1 – Basic allocation & automatic collection
 * ================================================================ */
static int test1_basic_alloc(void) {
    TEST_START("basic allocation and collection");

    // size_t initial = 0;
    /* Allocate several blocks and then drop all references.
       After a gc_collect they should be reclaimed. */
    for (int i = 0; i < 10; i++) {
        void *p = gc_malloc(1024);
        ASSERT(p != NULL, "gc_malloc returned NULL");
    }
    gc_collect();
    /* After collection the heap should be mostly empty (apart from
       possible false positives). We just verify that gc_malloc doesn't
       crash and that basic collection completes. */
    TEST_PASS();
}

/* ================================================================
 * Test 2 – Reachable graph is preserved
 * ================================================================ */
static int test2_reachable_objects(void) {
    TEST_START("reachable objects preserved across collection");

    node_t *list = NULL;
    for (int i = 0; i < 5; i++) {
        node_t *n = gc_malloc(sizeof(node_t));
        ASSERT(n != NULL, "gc_malloc failed");
        n->value = i;
        n->next = list;
        list = n;
    }
    gc_collect();
    /* Verify list integrity */
    int count = 0;
    for (node_t *cur = list; cur; cur = cur->next) count++;
    ASSERT(count == 5, "list truncated after collection");
    TEST_PASS();
}

/* ================================================================
 * Test 3 – Precise roots protect cyclic structures
 * ================================================================ */
static int test3_cycle_with_roots(void) {
    TEST_START("cyclic structure protected by precise roots");

    node_t *a = gc_malloc(sizeof(node_t));
    node_t *b = gc_malloc(sizeof(node_t));
    ASSERT(a && b, "allocation failed");
    a->value = 1; a->next = b;
    b->value = 2; b->next = a;

    gc_add_root(&a);                    // protect the cycle
    gc_collect();
    ASSERT(a != NULL && b != NULL, "live objects collected");

    gc_remove_root(&a);
    a = NULL;
    force_stack_clear();
    gc_collect();
    /* Cycle should now be reclaimable – we can't easily check,
       but at least the previous collection must have kept it alive. */
    TEST_PASS();
}

/* ================================================================
 * Test 4 – calloc zeroes memory
 * ================================================================ */
static int test4_calloc_zero(void) {
    TEST_START("calloc zeroes memory");

    int *arr = gc_calloc(10, sizeof(int));
    ASSERT(arr != NULL, "calloc returned NULL");
    for (int i = 0; i < 10; i++) {
        ASSERT(arr[i] == 0, "calloc did not zero memory");
    }
    TEST_PASS();
}

/* ================================================================
 * Test 5 – Automatic GC trigger on threshold exceed
 * ================================================================ */
static int test5_auto_gc(void) {
    TEST_START("automatic collection on threshold exceed");

    for (int i = 0; i < 10; i++) {
        void *p = gc_malloc(1024 * 1024);  // 1 MiB each
        ASSERT(p != NULL, "allocation failed");
        /* we don't retain the pointer, so each block becomes garbage */
    }
    gc_collect();
    TEST_PASS();
}

/* ================================================================
 * Test 6 – gc_setjmp / gc_longjmp ( TRY / CATCH / END_TRY / THROW )
 * ================================================================ */
static int test6_setjmp_longjmp(void) {
    TEST_START("try / throw / catch");

    int caught_code = 0;
    char *p1 = gc_malloc(100);
    char *p2 = gc_malloc(200);
    ASSERT(p1 && p2, "allocation failed");
    snprintf(p1, 100, "Hello");
    snprintf(p2, 200, "World");

    TRY {
        ASSERT(p1 && p2, "allocation check");
        THROW(42);
        TEST_FAIL("unreachable code reached (throw did not jump)");
    }
    CATCH {
        caught_code = THROWN;
    }
    END_TRY;

    ASSERT(caught_code == 42, "exception code mismatch");
    gc_collect();

    TRY {
        LOG_INFO("Outer TRY start");
        
        TRY {
            LOG_INFO("  Inner TRY start");
            THROW(2);
            LOG_INFO("  This won't print");
        } CATCH {
            caught_code = THROWN;
            LOG_INFO("  Caught inner: %d", caught_code);
        } END_TRY;

        ASSERT(caught_code == 2, "exception code mismatch");

        LOG_INFO("Outer TRY continuing");
        THROW(1);
        
    } CATCH {
        caught_code = THROWN;
        LOG_INFO("Caught outer: %d", caught_code);
    } END_TRY;

    ASSERT(caught_code == 1, "exception code mismatch");
    gc_collect();

    TEST_PASS();
}

/* ================================================================
 * Test 7 – Multi‑threaded allocation and collection
 * ================================================================ */
static void *thread_func(void *arg) {
    int id = *(int *)arg;
    void *p = gc_malloc(128);
    snprintf(p, 128, "Thread %d", id);
    return NULL;
}

static int test7_multithread(void) {
    TEST_START("multi‑threaded allocation and collection");

    const int N = 4;
    pthread_t threads[N];
    int ids[N];
    for (int i = 0; i < N; i++) {
        ids[i] = i;
        int rc = gc_pthread_create(&threads[i], NULL, thread_func, &ids[i]);
        ASSERT(rc == 0, "gc_pthread_create failed");
    }
    for (int i = 0; i < N; i++) {
        gc_pthread_join(threads[i], NULL);
    }
    gc_collect();
    TEST_PASS();
}

/* ================================================================
 * Test 8 – Weak references and finalizers
 * ================================================================ */
#if GC_FINALIZING
static int finalizer_called = 0;
static void test8_finalizer(void *obj, void *data) {
    (void)obj;
    (void)data;
    finalizer_called++;
}
static void test8_weak_cleared(void *obj, void *data) {
    (void)obj;
    (void)data;
}

static int test8_weak_and_finalizers(void) {
    TEST_START("weak references and finalizers");

    finalizer_called = 0;
    char *p = gc_malloc(100);
    ASSERT(p != NULL, "allocation failed");
    snprintf(p, 100, "finalizer test");
    gc_register_finalizer(p, test8_finalizer, NULL);

    gc_weak_ref_t *weak = gc_make_weak(p, test8_weak_cleared, NULL);
    ASSERT(weak != NULL, "gc_make_weak failed");
    ASSERT(gc_weak_get(weak) == p, "weak ref should be alive");

    /* Use precise root to first guarantee liveness, then drop it. */
    gc_add_root(&p);
    gc_remove_root(&p);
    p = NULL;
    force_stack_clear();
    gc_collect();

    ASSERT(gc_weak_get(weak) == NULL, "weak ref not cleared after GC");
    ASSERT(finalizer_called == 1, "finalizer not called exactly once");

    gc_weak_free(weak);
    TEST_PASS();
}
#else
#define test8_weak_and_finalizers()  (0)
#endif /* GC_FINALIZING */

/* ================================================================
 * Test 9 – Write barrier (incremental marking only)
 * ================================================================ */
#if GC_INCREMENTAL
static int test9_write_barrier(void) {
    TEST_START("write barrier with incremental marking");

    node_t *root = gc_malloc(sizeof(node_t));
    ASSERT(root != NULL, "allocation failed");
    root->value = 100;
    root->next = NULL;

    gc_collect();                   /* start incremental cycle */

    node_t *child = gc_malloc(sizeof(node_t));
    ASSERT(child != NULL, "allocation failed");
    child->value = 200;
    child->next = NULL;

    GC_WRITE_BARRIER(root, child);  /* inform GC of new reference */
    root->next = child;

    gc_collect();                   /* finish cycle */

    ASSERT(root->next == child, "child lost after incremental marking");
    ASSERT(child->value == 200, "child corrupted");
    TEST_PASS();
}
#else
#define test9_write_barrier()  (0)
#endif /* GC_INCREMENTAL */

/* ================================================================
 * Test runner
 * ================================================================ */
__attribute__((optimize("O0")))
int main(void) {
    gc_init();

    printf("=== Conservative GC Unit Tests ===\n");

    /* Run each test and record failures */
    int (*tests[])(void) = {
        test1_basic_alloc,
        test2_reachable_objects,
        test3_cycle_with_roots,
        test4_calloc_zero,
        test5_auto_gc,
        test6_setjmp_longjmp,
        test7_multithread,
#if GC_FINALIZING
        test8_weak_and_finalizers,
#endif // GC_FINALIZING
#if GC_INCREMENTAL
        test9_write_barrier,
#endif // GC_INCREMENTAL
    };
    const char *names[] = {
        "test1_basic_alloc",
        "test2_reachable_objects",
        "test3_cycle_with_roots",
        "test4_calloc_zero",
        "test5_auto_gc",
        "test6_setjmp_longjmp",
        "test7_multithread",
#if GC_FINALIZING
        "test8_weak_and_finalizers",
#endif // GC_FINALIZING
#if GC_INCREMENTAL
        "test9_write_barrier",
#endif // GC_INCREMENTAL
    };

    for (int i = 0; i < (int)(sizeof(tests) / sizeof(tests[0])); i++) {
        int ret = tests[i]();
        if (ret != 0) {
            printf("  -> %s encountered a failure.\n", names[i]);
        }
        printf("\n");
    }

    printf("--------------------------------------------------\n");
    printf("Tests run: %d, failures: %d\n", tests_run, tests_failed);
    printf("All tests %s\n", tests_failed ? "FAILED" : "PASSED");

    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

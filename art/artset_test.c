
// artset_test.c
// 编译: gcc -g -std=c11 -fPIC -Wall -Wextra -o artset_test artset.c artset_test.c

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "artset.h"

// 辅助函数：验证键是否存在且值正确
static void check(ASET *t, uint64_t key) {
    assert(aset_contains(t, key) == 1);
}

static void check_not_found(ASET *t, uint64_t key) {
    assert(aset_contains(t, key) == 0);
}

// 测试1：基本插入与查找（无冲突）
static void test_basic() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    for (uint64_t i = 10; i <= 100; i += 10)
        aset_insert(&t, i);

    for (uint64_t i = 10; i <= 100; i += 10)
        check(&t, i);

    check_not_found(&t, 5);
    check_not_found(&t, 110);
}

// 测试2：更新已存在的键
static void test_update() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    aset_insert(&t, 42);
    check(&t, 42);

    aset_insert(&t, 42);
    check(&t, 42);
}

// 测试3：不同键共享前缀时应正确分裂叶子（当前实现会失败）
static void test_collision() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    aset_insert(&t, 0x0000000000000011);   // 低位字节 0x11
    aset_insert(&t, 0x0000000000000022);   // 低位字节 0x22
    // 两个键的第一个字节都是 0x11 或 0x22？在 little‑endian 下首个字节就是低位
    // 但当前实现因字节序问题根本找不到，这里实际测试查找
    check(&t, 0x0000000000000011);
    check(&t, 0x0000000000000022);
}

// 测试4：字节序问题（在 little‑endian 机器上会暴露）
static void test_endianness() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    uint64_t k1 = 0x0102030405060708;
    uint64_t k2 = 0x0807060504030201;
    aset_insert(&t, k1);
    aset_insert(&t, k2);
    check(&t, k1);
    check(&t, k2);
}

// 测试5：删除操作及节点收缩
static void test_delete() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    // 插入一批键，迫使内部节点扩展
    for (uint64_t i = 0; i < 200; i++) {
        aset_insert(&t, i);
    }
    // 删除一些键，触发 shrink
    for (uint64_t i = 1; i < 100; i++) {
        assert(aset_delete_with_shrink(&t, i) == 0);
    }
    for (uint64_t i = 0; i < 100; i++) {
        assert(aset_contains(&t, i) == 0);
        check_not_found(&t, i);
        assert(aset_delete_with_shrink(&t, i) == -1);
    }
    // 剩下的键应该还在
    for (uint64_t i = 100; i < 200; i++) {
        check(&t, i);
    }
    // 删除不存在的键
    assert(aset_delete_with_shrink(&t, 1000) == -1);
}

// 测试7：大量随机键
static void test_random() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    const int N = 100000;
    uint64_t *keys = malloc(N * sizeof(uint64_t));
    srand(42);
    for (int i = 0; i < N; i++) {
        keys[i] = ((uint64_t)rand() << 32) | rand();
        // 允许零键，但零键也是有效键，不跳过
        aset_insert(&t, keys[i]);
    }
    for (int i = 0; i < N; i++) {
        check(&t, keys[i]);
    }
    // 随机删除一半
    for (int i = 0; i < N/2; i++) {
        int idx = rand() % N;
        if (keys[idx] != 0) { // 只删除尚未被标记删除的
            aset_delete_with_shrink(&t, keys[idx]);
            keys[idx] = 0;
        }
    }
    for (int i = 0; i < N; i++) {
        if (keys[i] != 0)
            check(&t, keys[i]);
        else
            check_not_found(&t, keys[i]);
    }
    free(keys);
}

// 测试 next / prev
static void test_next_prev() {
    ASET t = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    uint64_t keys[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    int nkeys = sizeof(keys)/sizeof(keys[0]);

    for (int i = 0; i < nkeys; i++)
        aset_insert(&t, keys[i]);

    uint64_t res;

    // 1. 测试存在 key 的后继
    res = aset_next(&t, 30);
    assert(res == 40);
    res = aset_next(&t, 100);
    assert(res == 0);   // 最大键无后继
    res = aset_next(&t, 5);
    assert(res == 10);
    res = aset_next(&t, 95);
    assert(res == 100);
    res = aset_next(&t, 101);
    assert(res == 0);

    // 2. 测试存在 key 的前驱
    res = aset_prev(&t, 30);
    assert(res == 20);
    res = aset_prev(&t, 10);
    assert(res == 0);   // 最小键无前驱
    res = aset_prev(&t, 5);
    assert(res == 0);
    res = aset_prev(&t, 95);
    assert(res == 90);
    res = aset_prev(&t, 101);
    assert(res == 100);

    // 3. 测试不存在 key 的后继/前驱
    // 插入 25 和 35 后，使得路径中有中间节点，测试更复杂的树结构
    aset_insert(&t, 25);
    aset_insert(&t, 35);

    res = aset_next(&t, 27);
    assert(res == 30);
    res = aset_prev(&t, 27);
    assert(res == 25);

    res = aset_next(&t, 37);
    assert(res == 40);
    res = aset_prev(&t, 37);
    assert(res == 35);

    // 4. 测试空树
    ASET empty = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    res = aset_next(&empty, 100);
    assert(res == 0);
    res = aset_prev(&empty, 100);
    assert(res == 0);

    // 5. 测试只有一个键的情况
    ASET single = {.root = 0, .conf = &(aset_conf){malloc, calloc, free}};
    aset_insert(&single, 42);
    res = aset_next(&single, 42);
    assert(res == 0);
    res = aset_prev(&single, 42);
    assert(res == 0);
    res = aset_next(&single, 41);
    assert(res == 42);
    res = aset_prev(&single, 43);
    assert(res == 42);

    // 清理（如果需要）
    // 实际测试中树会局部释放，这里不重复释放也没问题

    printf("test_next_prev passed.\n");
}

int main(void) {
    printf("Running tests...\n");
    test_basic();
    printf("test_basic passed.\n");
    test_update();
    printf("test_update passed.\n");
    test_collision();   // 预期在此失败
    printf("test_collision passed.\n");
    test_endianness();  // 预期失败（little‑endian）
    printf("test_endianness passed.\n");
    test_delete();      // 可能崩溃或失败
    printf("test_delete passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_random();
    printf("test_random passed.\n");
    test_next_prev();
    printf("All tests passed!\n");
    return 0;
}

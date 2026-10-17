#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memlib.h"
#include "mm.h"

static int tests_run = 0, tests_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "    FAILED: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            failed = 1;                                                       \
        }                                                                     \
    } while (0)

static void fresh_heap(void) {
    mem_reset_brk();
    if (mm_init() != 0) {
        fprintf(stderr, "mm_init failed\n");
        exit(1);
    }
}

static int aligned(void *p) { return ((uintptr_t)p % 16) == 0; }

static int test_alignment(void) {
    int failed = 0;
    for (uint32_t size = 1; size <= 300; size++) {
        void *p = mm_malloc(size);
        EXPECT(p != NULL);
        EXPECT(aligned(p));
    }
    return failed;
}

static int test_zero_and_null(void) {
    int failed = 0;
    EXPECT(mm_malloc(0) == NULL);
    mm_free(NULL);
    void *p = mm_realloc(NULL, 40);
    EXPECT(p != NULL);
    EXPECT(mm_realloc(p, 0) == NULL);
    return failed;
}

static int test_write_read(void) {
    int failed = 0;
    unsigned char *a = mm_malloc(100), *b = mm_malloc(100);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 100);
    for (int i = 0; i < 100; i++) {
        EXPECT(a[i] == 0xAA);
        EXPECT(b[i] == 0xBB);
        if (failed) break;
    }
    return failed;
}

static int test_coalesce(void) {
    int failed = 0;
    int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (int o = 0; o < 6; o++) {
        fresh_heap();
        void *guard_before = mm_malloc(64);
        void *p[3] = {mm_malloc(48), mm_malloc(48), mm_malloc(48)};
        void *guard_after = mm_malloc(64);
        for (int k = 0; k < 3; k++) mm_free(p[orders[o][k]]);
        EXPECT(mm_checkheap(0) == 0);
        /* 3 x 64-byte blocks -> one 192-byte block */
        void *big = mm_malloc(180);
        EXPECT(big == p[0]);
        (void)guard_before;
        (void)guard_after;
    }
    return failed;
}

static int test_realloc_keeps_data(void) {
    int failed = 0;
    unsigned char *p = mm_malloc(10);
    for (int i = 0; i < 10; i++) p[i] = (unsigned char)i;
    void *blocker = mm_malloc(10);
    p = mm_realloc(p, 5000);
    for (int i = 0; i < 10; i++) EXPECT(p[i] == i);
    p = mm_realloc(p, 4);
    for (int i = 0; i < 4; i++) EXPECT(p[i] == i);
    (void)blocker;
    return failed;
}

static int test_realloc_in_place(void) {
    int failed = 0;
    void *a = mm_malloc(64);
    void *b = mm_malloc(64);
    void *c = mm_malloc(64);
    mm_free(b);
    void *a2 = mm_realloc(a, 120);
    EXPECT(a2 == a);
    void *c2 = mm_realloc(c, 100000);
    EXPECT(c2 == c);
    return failed;
}

static int test_realloc_slides_back(void) {
    int failed = 0;
    /* all < BIG_BLOCK so they're placed contiguously */
    void *a = mm_malloc(100);
    char *b = mm_malloc(40);
    void *guard = mm_malloc(40);
    strcpy(b, "hello allocator");
    mm_free(a);
    char *b2 = mm_realloc(b, 150);
    EXPECT(b2 == a);
    EXPECT(strcmp(b2, "hello allocator") == 0);
    (void)guard;
    return failed;
}

static int test_reuses_freed_memory(void) {
    int failed = 0;
    void *p = mm_malloc(1000);
    mm_free(p);
    size_t before = mem_heapsize();
    for (int i = 0; i < 1000; i++) {
        void *q = mm_malloc(1000);
        mm_free(q);
    }
    EXPECT(mem_heapsize() == before);
    return failed;
}

static int test_large(void) {
    int failed = 0;
    unsigned char *p = mm_malloc(50u << 20);
    EXPECT(p != NULL);
    if (p) {
        p[0] = 1;
        p[(50u << 20) - 1] = 2;
        EXPECT(p[0] == 1 && p[(50u << 20) - 1] == 2);
    }
    mm_free(p);
    return failed;
}

static int test_huge_requests(void) {
    int failed = 0;
    uint32_t huge[] = {0xFFFFFFFFu, 0xFFFFFFFCu, 0xFFFFFFF0u, 0x80000000u, 0x7FFFFFFFu};
    for (int i = 0; i < 5; i++) EXPECT(mm_malloc(huge[i]) == NULL);

    char *p = mm_malloc(32);
    strcpy(p, "still here");
    EXPECT(mm_realloc(p, 0xFFFFFFF8u) == NULL);
    EXPECT(strcmp(p, "still here") == 0);
    mm_free(p);
    return failed;
}

static int test_out_of_memory(void) {
    int failed = 0;
    void *a = mm_malloc(600u << 20);
    EXPECT(a != NULL);
    void *b = mm_malloc(600u << 20);
    EXPECT(b == NULL);
    EXPECT(mm_checkheap(0) == 0);
    void *c = mm_malloc(100);
    EXPECT(c != NULL);
    char *d = mm_malloc(64);
    strcpy(d, "keep me");
    EXPECT(mm_realloc(d, 700u << 20) == NULL);
    EXPECT(strcmp(d, "keep me") == 0);
    mm_free(a);
    mm_free(c);
    mm_free(d);
    return failed;
}

static int test_checker_catches_corruption(void) {
    int failed = 0;
    void *a = mm_malloc(64);
    void *b = mm_malloc(64);
    mm_malloc(64);
    mm_free(b);
    EXPECT(mm_checkheap(-1) == 0);

    uint32_t *b_header = (uint32_t *)((char *)b - 4);
    uint32_t saved = *b_header;
    *b_header ^= 0x40;
    EXPECT(mm_checkheap(-1) > 0);
    *b_header = saved;

    uint32_t *a_header = (uint32_t *)((char *)a - 4);
    saved = *a_header;
    *a_header &= ~1u;
    EXPECT(mm_checkheap(-1) > 0);
    *a_header = saved;

    EXPECT(mm_checkheap(-1) == 0);
    return failed;
}

#define SLOTS 2000

typedef struct {
    unsigned char *p;
    uint32_t size;
    unsigned char tag;
} Slot;

static uint32_t rng = 12345;
static uint32_t rand32(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint32_t random_size(void) {
    uint32_t r = rand32() % 100;
    if (r < 60) return 1 + rand32() % 64;
    if (r < 90) return 1 + rand32() % 1024;
    return 1 + rand32() % 65536;
}

/* each live block is filled with its tag byte, so any overlap shows up here */
static int verify(Slot *s) {
    for (uint32_t i = 0; i < s->size; i++)
        if (s->p[i] != s->tag) return 0;
    return 1;
}

static int test_stress(void) {
    int failed = 0;
    static Slot slots[SLOTS];
    memset(slots, 0, sizeof(slots));
    const int OPS = 300000;

    for (int op = 0; op < OPS && !failed; op++) {
        Slot *s = &slots[rand32() % SLOTS];
        uint32_t action = rand32() % 10;

        if (!s->p) {
            s->size = random_size();
            s->p = mm_malloc(s->size);
            EXPECT(s->p != NULL && aligned(s->p));
            s->tag = (unsigned char)(rand32() | 1);
            memset(s->p, s->tag, s->size);
        } else if (action < 5) {
            EXPECT(verify(s));
            mm_free(s->p);
            s->p = NULL;
        } else {
            EXPECT(verify(s));
            uint32_t new_size = random_size();
            s->p = mm_realloc(s->p, new_size);
            EXPECT(s->p != NULL && aligned(s->p));
            uint32_t kept = new_size < s->size ? new_size : s->size;
            for (uint32_t i = 0; i < kept && !failed; i++) EXPECT(s->p[i] == s->tag);
            s->size = new_size;
            memset(s->p, s->tag, s->size);
        }
        if (op % 5000 == 0) EXPECT(mm_checkheap(0) == 0);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].p) {
            EXPECT(verify(&slots[i]));
            mm_free(slots[i].p);
        }
    }
    EXPECT(mm_checkheap(0) == 0);
    return failed;
}

static void run(const char *name, int (*test)(void)) {
    fresh_heap();
    int failed = test();
    if (mm_checkheap(0) != 0) failed = 1;
    tests_run++;
    tests_failed += failed;
    printf("  %s  %s\n", failed ? "FAIL" : "pass", name);
}

int main(void) {
    mem_init();
    printf("Allocator tests\n");
    run("16-byte alignment for sizes 1..300", test_alignment);
    run("malloc(0), free(NULL), realloc edge cases", test_zero_and_null);
    run("blocks don't overlap", test_write_read);
    run("freeing neighbors merges them (all 6 orders)", test_coalesce);
    run("realloc keeps data when growing and shrinking", test_realloc_keeps_data);
    run("realloc grows in place when it can", test_realloc_in_place);
    run("realloc slides back into a free neighbor", test_realloc_slides_back);
    run("freed memory gets reused", test_reuses_freed_memory);
    run("50 MB allocation", test_large);
    run("near-4 GB requests fail instead of wrapping around", test_huge_requests);
    run("out of memory: NULL, and the heap stays healthy", test_out_of_memory);
    run("heap checker catches corruption", test_checker_catches_corruption);
    run("random stress: 300k mixed operations", test_stress);
    printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
    mem_deinit();
    return tests_failed ? 1 : 0;
}

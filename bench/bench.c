/* Replays synthetic workloads against mm and glibc malloc. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "memlib.h"
#include "mm.h"

typedef enum { OP_ALLOC, OP_FREE, OP_REALLOC } OpType;
typedef struct {
    OpType type;
    uint32_t id;
    uint32_t size;
} Op;

typedef struct {
    const char *name;
    Op *ops;
    size_t count, cap;
    uint32_t ids;
} Workload;

static void push(Workload *w, OpType t, uint32_t id, uint32_t size) {
    if (w->count == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 1024;
        w->ops = realloc(w->ops, w->cap * sizeof(Op));
    }
    w->ops[w->count++] = (Op){t, id, size};
    if (id + 1 > w->ids) w->ids = id + 1;
}

static uint32_t rng = 42;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static Workload small_objects(void) {
    Workload w = {.name = "small objects"};
    enum { LIVE = 10000, ROUNDS = 400000 };
    for (uint32_t i = 0; i < LIVE; i++) push(&w, OP_ALLOC, i, 8 + rnd() % 120);
    for (uint32_t r = 0; r < ROUNDS; r++) {
        uint32_t id = rnd() % LIVE;
        push(&w, OP_FREE, id, 0);
        push(&w, OP_ALLOC, id, 8 + rnd() % 120);
    }
    for (uint32_t i = 0; i < LIVE; i++) push(&w, OP_FREE, i, 0);
    return w;
}

static Workload fragmentation_trap(void) {
    Workload w = {.name = "fragmentation trap"};
    enum { N = 20000 };
    for (uint32_t i = 0; i < N; i++) {
        push(&w, OP_ALLOC, 2 * i, 16);
        push(&w, OP_ALLOC, 2 * i + 1, 112);
    }
    for (uint32_t i = 0; i < N; i++) push(&w, OP_FREE, 2 * i + 1, 0);
    for (uint32_t i = 0; i < N; i++) push(&w, OP_ALLOC, 2 * N + i, 128);
    for (uint32_t i = 0; i < 3 * N; i++)
        if (i % 2 == 0 || i >= 2 * N) push(&w, OP_FREE, i, 0);
    return w;
}

static Workload growing_buffer(void) {
    Workload w = {.name = "growing buffer (realloc)"};
    enum { STEPS = 20000 };
    push(&w, OP_ALLOC, 0, 512);
    for (uint32_t i = 1; i <= STEPS; i++) {
        push(&w, OP_ALLOC, i, 128);
        push(&w, OP_REALLOC, 0, 512 + 128 * i);
        push(&w, OP_FREE, i, 0);
    }
    push(&w, OP_FREE, 0, 0);
    return w;
}

static Workload random_mix(void) {
    Workload w = {.name = "random mix"};
    enum { SLOTS = 4000, OPS = 400000 };
    static uint8_t live[SLOTS];
    memset(live, 0, sizeof(live));
    for (uint32_t k = 0; k < OPS; k++) {
        uint32_t id = rnd() % SLOTS;
        uint32_t r = rnd() % 100;
        uint32_t size = r < 70 ? 1 + rnd() % 256 : r < 95 ? 1 + rnd() % 4096 : 1 + rnd() % 65536;
        if (!live[id]) {
            push(&w, OP_ALLOC, id, size);
            live[id] = 1;
        } else if (rnd() % 3) {
            push(&w, OP_FREE, id, 0);
            live[id] = 0;
        } else {
            push(&w, OP_REALLOC, id, size);
        }
    }
    for (uint32_t id = 0; id < SLOTS; id++)
        if (live[id]) push(&w, OP_FREE, id, 0);
    return w;
}

static Workload large_blocks(void) {
    Workload w = {.name = "large blocks"};
    enum { SLOTS = 200, OPS = 40000 };
    static uint8_t live[SLOTS];
    memset(live, 0, sizeof(live));
    for (uint32_t k = 0; k < OPS; k++) {
        uint32_t id = rnd() % SLOTS;
        if (!live[id]) {
            push(&w, OP_ALLOC, id, 4096 + rnd() % (1 << 20));
            live[id] = 1;
        } else {
            push(&w, OP_FREE, id, 0);
            live[id] = 0;
        }
    }
    for (uint32_t id = 0; id < SLOTS; id++)
        if (live[id]) push(&w, OP_FREE, id, 0);
    return w;
}

typedef struct {
    const char *name;
    void (*reset)(void);
    void *(*alloc)(uint32_t);
    void (*release)(void *);
    void *(*resize)(void *, uint32_t);
} Allocator;

static void mm_reset(void) {
    mem_reset_brk();
    mm_init();
}
static void libc_reset(void) {}
static void *libc_alloc(uint32_t n) { return malloc(n); }
static void *libc_resize(void *p, uint32_t n) { return realloc(p, n); }

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double replay(const Workload *w, const Allocator *a, size_t *peak_live) {
    void **ptrs = calloc(w->ids, sizeof(void *));
    uint32_t *sizes = calloc(w->ids, sizeof(uint32_t));
    size_t live = 0, peak = 0;

    a->reset();
    double start = now();
    for (size_t i = 0; i < w->count; i++) {
        const Op *op = &w->ops[i];
        switch (op->type) {
        case OP_ALLOC:
            ptrs[op->id] = a->alloc(op->size);
            ((char *)ptrs[op->id])[0] = 1;
            live += op->size;
            sizes[op->id] = op->size;
            break;
        case OP_FREE:
            a->release(ptrs[op->id]);
            live -= sizes[op->id];
            break;
        case OP_REALLOC:
            ptrs[op->id] = a->resize(ptrs[op->id], op->size);
            live = live - sizes[op->id] + op->size;
            sizes[op->id] = op->size;
            break;
        }
        if (live > peak) peak = live;
    }
    double secs = now() - start;
    if (peak_live) *peak_live = peak;
    free(ptrs);
    free(sizes);
    return secs;
}

int main(void) {
    mem_init();
    Allocator mm = {"mm", mm_reset, mm_malloc, mm_free, mm_realloc};
    Allocator libc = {"glibc", libc_reset, libc_alloc, free, libc_resize};

    Workload workloads[] = {small_objects(), fragmentation_trap(), growing_buffer(), random_mix(), large_blocks()};
    int n = sizeof(workloads) / sizeof(workloads[0]);

    printf("%-26s %9s %13s %13s %8s %11s\n", "workload", "ops", "mm Mops/s", "glibc Mops/s", "mm/glibc",
           "mm util");
    for (int i = 0; i < n; i++) {
        Workload *w = &workloads[i];
        double best_mm = 1e9, best_libc = 1e9;
        size_t peak = 0;
        for (int run = 0; run < 5; run++) {
            double t = replay(w, &mm, &peak);
            if (t < best_mm) best_mm = t;
            t = replay(w, &libc, NULL);
            if (t < best_libc) best_libc = t;
        }
        /* the simulated heap never shrinks, so its final size is the peak */
        replay(w, &mm, &peak);
        double util = 100.0 * peak / mem_heapsize();
        if (mm_checkheap(0) != 0) printf("  (heap check failed!)\n");

        double mm_rate = w->count / best_mm / 1e6, libc_rate = w->count / best_libc / 1e6;
        printf("%-26s %9zu %13.1f %13.1f %7.2fx %10.0f%%\n", w->name, w->count, mm_rate, libc_rate, mm_rate / libc_rate,
               util);
    }
    mem_deinit();
    return 0;
}

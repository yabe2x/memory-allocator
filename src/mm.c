/*
 * Segregated explicit free lists with boundary-tag coalescing, 16-byte aligned.
 *
 * Header (4 bytes): size | PA | A    (A = allocated, PA = previous block allocated)
 *   allocated: [hdr | payload ...]
 *   free:      [hdr | next | prev | ... | ftr]
 *
 * Free blocks link through their payload using 4-byte heap offsets, so the
 * minimum block is 16 bytes. Allocated blocks have no footer; the PA bit
 * replaces it. The 16 list heads are stored at the start of the heap.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memlib.h"
#include "mm.h"

#define WSIZE 4
#define ALIGNMENT 16
#define MIN_BLOCK 16
/* tunable with -D */
#ifndef CHUNKSIZE
#define CHUNKSIZE (1 << 12)
#endif
#ifndef FIT_SEARCH
#define FIT_SEARCH 8  /* extra candidates checked after the first fit */
#endif
#ifndef BIG_BLOCK
#define BIG_BLOCK 128  /* blocks this big are placed at the end of a free block */
#endif
#ifndef EXACT_TAIL_GROW
#define EXACT_TAIL_GROW 1
#endif

#define NUM_CLASSES 16

#define ALLOC 0x1
#define PREV_ALLOC 0x2

#define HEADS_SIZE (NUM_CLASSES * WSIZE)
#define FIRST_PAYLOAD (HEADS_SIZE + ALIGNMENT)

int mm_checkheap(int verbose);

static char *heap_lo;
static uint32_t *list_heads;

static inline uint32_t align_up(uint32_t n) { return (n + (ALIGNMENT - 1)) & ~(uint32_t)(ALIGNMENT - 1); }

static inline uint32_t get(const void *p) { return *(const uint32_t *)p; }
static inline void put(void *p, uint32_t val) { *(uint32_t *)p = val; }

static inline uint32_t pack(uint32_t size, uint32_t bits) { return size | bits; }

static inline void *hdrp(void *bp) { return (char *)bp - WSIZE; }
static inline uint32_t block_size(void *bp) { return get(hdrp(bp)) & ~0xFu; }
static inline int is_alloc(void *bp) { return get(hdrp(bp)) & ALLOC; }
static inline int prev_is_alloc(void *bp) { return (get(hdrp(bp)) & PREV_ALLOC) != 0; }
static inline void *ftrp(void *bp) { return (char *)bp + block_size(bp) - 2 * WSIZE; }
static inline void *next_blkp(void *bp) { return (char *)bp + block_size(bp); }
/* only valid when the previous block is free */
static inline void *prev_blkp(void *bp) { return (char *)bp - (get((char *)bp - 2 * WSIZE) & ~0xFu); }

static inline void set_prev_alloc(void *bp, int alloc) {
    uint32_t h = get(hdrp(bp));
    put(hdrp(bp), alloc ? (h | PREV_ALLOC) : (h & ~(uint32_t)PREV_ALLOC));
}

static inline void write_free(void *bp, uint32_t size, int prev_alloc) {
    uint32_t h = pack(size, prev_alloc ? PREV_ALLOC : 0);
    put(hdrp(bp), h);
    put((char *)bp + size - 2 * WSIZE, h);
}

static inline void *from_off(uint32_t off) { return off ? heap_lo + off : NULL; }
static inline uint32_t to_off(void *bp) { return bp ? (uint32_t)((char *)bp - heap_lo) : 0; }
static inline void *next_free(void *bp) { return from_off(get(bp)); }
static inline void *prev_free(void *bp) { return from_off(get((char *)bp + WSIZE)); }
static inline void set_next_free(void *bp, void *n) { put(bp, to_off(n)); }
static inline void set_prev_free(void *bp, void *p) { put((char *)bp + WSIZE, to_off(p)); }

/* 16-byte classes up to 128, then powers of two */
static int size_class(uint32_t size) {
    if (size <= 128) return (int)(size / 16) - 1;
    int c = 8;
    uint32_t limit = 256;
    while (c < NUM_CLASSES - 1 && size > limit) {
        limit <<= 1;
        c++;
    }
    return c;
}

static void insert_free(void *bp) {
    int c = size_class(block_size(bp));
    void *head = from_off(list_heads[c]);
    set_next_free(bp, head);
    set_prev_free(bp, NULL);
    if (head) set_prev_free(head, bp);
    list_heads[c] = to_off(bp);
}

static void remove_free(void *bp) {
    void *prev = prev_free(bp);
    void *next = next_free(bp);
    if (prev)
        set_next_free(prev, next);
    else
        list_heads[size_class(block_size(bp))] = to_off(next);
    if (next) set_prev_free(next, prev);
}

static void *coalesce(void *bp) {
    uint32_t size = block_size(bp);
    void *next = next_blkp(bp);
    int prev_alloc = prev_is_alloc(bp);
    int next_alloc = is_alloc(next);

    if (!next_alloc) {
        remove_free(next);
        size += block_size(next);
    }
    if (!prev_alloc) {
        void *prev = prev_blkp(bp);
        remove_free(prev);
        size += block_size(prev);
        bp = prev;
    }
    write_free(bp, size, prev_is_alloc(bp));
    set_prev_alloc(next_blkp(bp), 0);
    insert_free(bp);
    return bp;
}

static void *extend_heap(uint32_t bytes) {
    char *bp = mem_sbrk((int)bytes);
    if (bp == (void *)-1) return NULL;

    write_free(bp, bytes, prev_is_alloc(bp));
    put(hdrp(next_blkp(bp)), pack(0, ALLOC));
    return coalesce(bp);
}

static void *last_free_block(void) {
    char *epilogue_bp = (char *)mem_heap_hi() + 1;
    if (prev_is_alloc(epilogue_bp)) return NULL;
    return prev_blkp(epilogue_bp);
}

static void *find_fit(uint32_t asize) {
    for (int c = size_class(asize); c < NUM_CLASSES; c++) {
        void *best = NULL;
        uint32_t best_size = 0;
        int extra = 0;
        for (void *bp = from_off(list_heads[c]); bp; bp = next_free(bp)) {
            uint32_t s = block_size(bp);
            if (s < asize) continue;
            if (!best || s < best_size) {
                best = bp;
                best_size = s;
                if (s == asize) break;
            }
            if (++extra > FIT_SEARCH) break;
        }
        if (best) return best;
    }
    return NULL;
}

/* Big blocks go at the end of the free block, small ones at the start, to keep
 * them apart and reduce fragmentation. */
static void *place(void *bp, uint32_t asize) {
    uint32_t csize = block_size(bp);
    uint32_t remainder = csize - asize;
    int prev_alloc = prev_is_alloc(bp);
    remove_free(bp);

    if (remainder < MIN_BLOCK) {
        put(hdrp(bp), pack(csize, ALLOC | (prev_alloc ? PREV_ALLOC : 0)));
        set_prev_alloc(next_blkp(bp), 1);
        return bp;
    }

    if (asize >= BIG_BLOCK) {
        write_free(bp, remainder, prev_alloc);
        insert_free(bp);
        void *ap = next_blkp(bp);
        put(hdrp(ap), pack(asize, ALLOC));
        set_prev_alloc(next_blkp(ap), 1);
        return ap;
    }

    put(hdrp(bp), pack(asize, ALLOC | (prev_alloc ? PREV_ALLOC : 0)));
    void *rest = next_blkp(bp);
    write_free(rest, remainder, 1);
    insert_free(rest);
    return bp;
}

/* larger requests would overflow size + header in 32 bits */
#define MAX_REQUEST (0x7FFFFFFFu - 2 * ALIGNMENT)

/* returns 0 if the request is too large */
static uint32_t adjust_size(uint32_t size) {
    if (size > MAX_REQUEST) return 0;
    uint32_t asize = align_up(size + WSIZE);
    return asize < MIN_BLOCK ? MIN_BLOCK : asize;
}

static void shrink_block(void *bp, uint32_t asize) {
    uint32_t size = block_size(bp);
    if (size - asize < MIN_BLOCK) return;
    put(hdrp(bp), pack(asize, ALLOC | (prev_is_alloc(bp) ? PREV_ALLOC : 0)));
    void *rest = next_blkp(bp);
    write_free(rest, size - asize, 1);
    set_prev_alloc(next_blkp(rest), 0);
    coalesce(rest);
}

/* -DMM_DEBUG checks the whole heap after every call */
#ifdef MM_DEBUG
#define CHECK_HEAP(what)                                               \
    do {                                                               \
        if (mm_checkheap(0)) {                                         \
            fprintf(stderr, "heap check failed after %s\n", what);     \
            abort();                                                   \
        }                                                              \
    } while (0)
#else
#define CHECK_HEAP(what)
#endif

static void *malloc_impl(uint32_t size);
static void free_impl(void *bp);
static void *realloc_impl(void *ptr, uint32_t size);

void *mm_malloc(uint32_t size) {
    void *p = malloc_impl(size);
    CHECK_HEAP("mm_malloc");
    return p;
}

void mm_free(void *bp) {
    free_impl(bp);
    CHECK_HEAP("mm_free");
}

void *mm_realloc(void *ptr, uint32_t size) {
    void *p = realloc_impl(ptr, size);
    CHECK_HEAP("mm_realloc");
    return p;
}

int mm_init(void) {
    heap_lo = mem_sbrk(FIRST_PAYLOAD);
    if (heap_lo == (void *)-1) return -1;
    if ((uintptr_t)heap_lo % ALIGNMENT) {
        fprintf(stderr, "mm_init: heap start is not %d-byte aligned\n", ALIGNMENT);
        return -1;
    }

    list_heads = (uint32_t *)heap_lo;
    for (int c = 0; c < NUM_CLASSES; c++) list_heads[c] = 0;
    put(heap_lo + FIRST_PAYLOAD - WSIZE, pack(0, ALLOC | PREV_ALLOC));

    if (extend_heap(CHUNKSIZE) == NULL) return -1;
    return 0;
}

static void *malloc_impl(uint32_t size) {
    if (size == 0) return NULL;
    uint32_t asize = adjust_size(size);
    if (asize == 0) return NULL;

    void *bp = find_fit(asize);
    if (!bp) {
        /* if the heap ends in a free block, only grow by the difference */
        void *last = last_free_block();
        uint32_t need = asize - (last ? block_size(last) : 0);
        uint32_t grow = need;
        if (!(EXACT_TAIL_GROW && last) && grow < CHUNKSIZE) grow = CHUNKSIZE;
        bp = extend_heap(grow);
        if (!bp) return NULL;
    }
    return place(bp, asize);
}

static void free_impl(void *bp) {
    if (!bp) return;
    write_free(bp, block_size(bp), prev_is_alloc(bp));
    set_prev_alloc(next_blkp(bp), 0);
    coalesce(bp);
}

/* Tries, in order: shrink in place, absorb the next block, extend the heap,
 * slide into the previous block, then fall back to malloc + copy. */
static void *realloc_impl(void *ptr, uint32_t size) {
    if (!ptr) return malloc_impl(size);
    if (size == 0) {
        free_impl(ptr);
        return NULL;
    }

    uint32_t asize = adjust_size(size);
    if (asize == 0) return NULL;
    uint32_t old = block_size(ptr);

    if (asize <= old) {
        shrink_block(ptr, asize);
        return ptr;
    }

    void *next = next_blkp(ptr);
    uint32_t next_free_size = is_alloc(next) ? 0 : block_size(next);
    int at_end = block_size(next) == 0 || (next_free_size && block_size(next_blkp(next)) == 0);

    if (at_end && old + next_free_size < asize) {
        if (!extend_heap(asize - old - next_free_size)) return NULL;
        next = next_blkp(ptr);
        next_free_size = block_size(next);
    }

    if (next_free_size && old + next_free_size >= asize) {
        remove_free(next);
        uint32_t total = old + next_free_size;
        put(hdrp(ptr), pack(total, ALLOC | (prev_is_alloc(ptr) ? PREV_ALLOC : 0)));
        set_prev_alloc(next_blkp(ptr), 1);
        shrink_block(ptr, asize);
        return ptr;
    }

    if (!prev_is_alloc(ptr)) {
        void *prev = prev_blkp(ptr);
        uint32_t total = block_size(prev) + old + next_free_size;
        if (total >= asize) {
            int prev_prev_alloc = prev_is_alloc(prev);
            remove_free(prev);
            if (next_free_size) remove_free(next);
            memmove(prev, ptr, old - WSIZE);
            put(hdrp(prev), pack(total, ALLOC | (prev_prev_alloc ? PREV_ALLOC : 0)));
            set_prev_alloc(next_blkp(prev), 1);
            shrink_block(prev, asize);
            return prev;
        }
    }

    void *newp = malloc_impl(size);
    if (!newp) return NULL;
    memcpy(newp, ptr, old - WSIZE);
    free_impl(ptr);
    return newp;
}

static int check_error(int verbose, const char *msg, void *bp) {
    if (verbose >= 0) fprintf(stderr, "heap check: %s (block %p)\n", msg, bp);
    return 1;
}

/* returns the number of problems found */
int mm_checkheap(int verbose) {
    int errors = 0;
    char *lo = heap_lo, *hi = (char *)mem_heap_hi();
    unsigned heap_free = 0, list_free = 0;
    int prev_alloc = 1;

    /* stop at the first bad size rather than follow it */
    char *bp = heap_lo + FIRST_PAYLOAD;
    int walk_ok = 1;
    for (; block_size(bp) > 0; bp = next_blkp(bp)) {
        uint32_t s = block_size(bp);
        if (verbose > 0)
            fprintf(stderr, "  %p  size %6u  %s\n", (void *)bp, s, is_alloc(bp) ? "allocated" : "free");
        if (s < MIN_BLOCK || s % ALIGNMENT || (char *)bp + s - WSIZE > hi + 1) {
            errors += check_error(verbose, "bad block size (stopping the heap walk)", bp);
            walk_ok = 0;
            break;
        }
        if ((uintptr_t)bp % ALIGNMENT) errors += check_error(verbose, "payload not 16-byte aligned", bp);
        if (prev_is_alloc(bp) != prev_alloc) errors += check_error(verbose, "PREV_ALLOC bit is wrong", bp);
        if (!is_alloc(bp)) {
            heap_free++;
            if (get(hdrp(bp)) != get(ftrp(bp))) errors += check_error(verbose, "header and footer differ", bp);
            if (!prev_alloc) errors += check_error(verbose, "two free blocks in a row (missed coalesce)", bp);
        }
        prev_alloc = is_alloc(bp);
    }
    if (walk_ok) {
        if (!is_alloc(bp)) errors += check_error(verbose, "bad epilogue", bp);
        if (prev_is_alloc(bp) != prev_alloc) errors += check_error(verbose, "epilogue PREV_ALLOC bit is wrong", bp);
        if ((char *)bp != hi + 1) errors += check_error(verbose, "epilogue is not at the end of the heap", bp);
    }

    for (int c = 0; c < NUM_CLASSES; c++) {
        void *prev = NULL;
        for (void *fp = from_off(list_heads[c]); fp; fp = next_free(fp)) {
            if ((char *)fp < lo || (char *)fp > hi) {
                errors += check_error(verbose, "free list points outside the heap", fp);
                break;
            }
            if (is_alloc(fp)) errors += check_error(verbose, "allocated block in a free list", fp);
            if (size_class(block_size(fp)) != c) errors += check_error(verbose, "block in the wrong size class", fp);
            if (prev_free(fp) != prev) errors += check_error(verbose, "free list prev link is wrong", fp);
            prev = fp;
            if (++list_free > mem_heapsize() / MIN_BLOCK) {
                errors += check_error(verbose, "free list has a cycle", fp);
                break;
            }
        }
    }
    if (walk_ok && list_free != heap_free)
        errors += check_error(verbose, "free block count: heap and lists disagree", NULL);
    return errors;
}

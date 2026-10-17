#include "memlib.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define MAX_HEAP (1u << 30)  /* 1 GB of address space (only touched pages use real memory) */

static char *heap_start;
static char *brk_ptr;
static char *heap_max;

void mem_init(void) {
    void *p = mmap(NULL, MAX_HEAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mem_init: mmap");
        exit(1);
    }
    heap_start = brk_ptr = p;
    heap_max = heap_start + MAX_HEAP;
}

void mem_deinit(void) {
    munmap(heap_start, MAX_HEAP);
    heap_start = brk_ptr = heap_max = NULL;
}

void *mem_sbrk(int incr) {
    if (incr < 0 || brk_ptr + incr > heap_max) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *old = brk_ptr;
    brk_ptr += incr;
    return old;
}

void mem_reset_brk(void) { brk_ptr = heap_start; }

void *mem_heap_lo(void) { return heap_start; }

void *mem_heap_hi(void) { return brk_ptr - 1; }

size_t mem_heapsize(void) { return (size_t)(brk_ptr - heap_start); }

#ifndef MM_H
#define MM_H

#include <stdint.h>

int mm_init(void);

void *mm_malloc(uint32_t size);
void mm_free(void *ptr);
void *mm_realloc(void *ptr, uint32_t size);

/* verbose: -1 silent, 0 print problems, 1 print every block. Returns the problem count. */
int mm_checkheap(int verbose);

#endif

# Dynamic Memory Allocator

My own `malloc`, `free`, and `realloc` in C. It manages a heap directly with
segregated explicit free lists, boundary-tag coalescing, and 16-byte alignment.
It's within 0.7–0.8× of glibc's malloc on everyday workloads and keeps about 95% of the heap in use.

It started as a CU Boulder systems lab, where my first version scored 69/100.
I redesigned it to reach **97/100**, with **141× the throughput** and **95% memory utilization**,
and added a test suite, a heap consistency checker, and a benchmark against glibc.

## Results

**The course's grading traces** (9 real program traces, 197k operations):

| | First version | This version |
|---|---|---|
| Free list | 1 explicit list, best fit | 16 segregated lists, bounded best fit |
| Alignment | 8-byte | **16-byte** (the x86-64 malloc guarantee) |
| Memory utilization | 69% | **95%** |
| Throughput | 546 K ops/s | **77,166 K ops/s (141×)** |
| Score | 69 / 100 | **97 / 100** |

**Against glibc malloc** (`make bench`; WSL2 Ubuntu, Ryzen 9 8945HS):

| Workload | Ops | mm (M ops/s) | glibc (M ops/s) | mm / glibc | mm utilization |
|---|---|---|---|---|---|
| Small objects (8–128 B churn) | 820k | 77.5 | 97.9 | 0.79× | 84% |
| Fragmentation trap | 120k | 67.2 | 21.3 | 3.16× | 81% |
| Growing buffer (realloc) | 60k | 96.9 | 140.4 | 0.69× | 100% |
| Random mix (1 B – 64 KB) | 402k | 15.8 | 21.4 | 0.74× | 77% |
| Large blocks (4 KB – 1 MB) | 40k | 12.0 | 1.0 | 11.9× | 91% |

This isn't a perfectly fair race. glibc is thread-safe and gets memory from the kernel (its
large-block result is mostly `mmap`/`munmap` system calls), while this allocator is
single-threaded and grows a simulated heap. The useful takeaway is that the data structures
hold up against a production allocator on ordinary workloads.

## How it works

Every block starts with a 4-byte header holding the block size and two flag bits:

```
 31                               4   3   2   1   0
+----------------------------------+---+---+---+---+
|  block size (multiple of 16)     |   |   |PA | A |     A  = allocated
+----------------------------------+---+---+---+---+     PA = previous block is allocated

Allocated:  [ header | payload ................................. ]
Free:       [ header | next | prev | ........ unused ..... | footer ]
```

- **Boundary tags.** Free blocks repeat their header as a footer at the end. When a block
  is freed, it looks at the footer just before it to find the previous block's start, so
  it can merge with free neighbors on both sides in O(1).
- **No footers on allocated blocks.** The `PA` bit tells a block whether its neighbor
  is allocated, so allocated blocks don't need a footer. That saves 4 bytes per allocation.
- **Segregated explicit free lists.** Free blocks are linked through their own unused
  payload into 16 lists by size: exact 16-byte classes up to 128 B, then powers of two.
  `malloc` starts at the smallest list that could fit and checks up to 8 extra candidates
  for a tighter fit, so it never walks the whole heap.
- **4-byte links.** List links are offsets from the heap start instead of 8-byte pointers,
  so the smallest free block is 16 bytes (header + next + prev + footer).
- **Split placement.** Small requests take the front of a free block and large ones
  (≥ 128 B) take the back. Keeping sizes apart reduces fragmentation a lot: without it,
  utilization on the `binary` trace drops from 91% to 54%.
- **Growing the heap.** `mem_sbrk()` moves the end of the heap forward, the way `sbrk()` does.
  If the heap already ends in a free block, it grows by only the missing bytes.
  That fixed the coalescing trace (66% → 99%).
- **realloc avoids copying.** In order, it tries to shrink in place, grow into a free next
  block, grow the heap under a block at the end, and slide back into a free previous block.
  Only if all of those fail does it move the block.

## Testing

```bash
make test     # 12 unit tests + a 300k-operation randomized stress test
make debug    # same tests with the full heap checked after EVERY operation,
              # plus AddressSanitizer and UndefinedBehaviorSanitizer
make bench    # benchmark against glibc
```

`mm_checkheap()` walks every block and every free list and checks that:
- payloads are aligned, and sizes are valid and stay inside the heap
- free blocks' headers match their footers
- no two free blocks sit next to each other (no coalescing was missed)
- every `PA` bit matches the real previous block
- every free list holds only free blocks of the right size class, with consistent prev/next links and no cycles
- the heap and the lists agree on how many blocks are free

A test deliberately corrupts the heap to make sure the checker catches it.
Other tests cover the edge cases: requests near 4 GB must fail instead of wrapping around to
a tiny block (a classic integer-overflow bug), running out of memory must return NULL and
leave the heap healthy, and a failed `realloc` must leave the original block untouched.
The stress test fills each block with a tag byte and checks the tags before every free and
realloc, so any overlap between blocks or any lost data shows up right away.

## Files

| File | What it is |
|---|---|
| `src/mm.c` | The allocator and heap checker |
| `src/memlib.c` | Simulated heap: reserves address space and grows it with `mem_sbrk()` |
| `tests/test_mm.c` | Unit tests and randomized stress test |
| `bench/bench.c` | Workload generator and benchmark vs glibc |

Build on Linux (or WSL) with `gcc` and `make`.

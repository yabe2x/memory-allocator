CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -Isrc
DEBUG   = -Wall -Wextra -O0 -g -Isrc -DMM_DEBUG -fsanitize=address,undefined

SRC = src/mm.c src/memlib.c

.PHONY: all test debug bench clean

all: test_mm bench_mm

test_mm: tests/test_mm.c $(SRC) src/mm.h src/memlib.h
	$(CC) $(CFLAGS) -o $@ tests/test_mm.c $(SRC)

bench_mm: bench/bench.c $(SRC) src/mm.h src/memlib.h
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(SRC)

test: test_mm
	./test_mm

# heap check after every call + ASan/UBSan
debug: tests/test_mm.c $(SRC)
	$(CC) $(DEBUG) -o test_mm_debug tests/test_mm.c $(SRC)
	./test_mm_debug

bench: bench_mm
	./bench_mm

clean:
	rm -f test_mm test_mm_debug bench_mm

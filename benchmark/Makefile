CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -I..

LIB_SOURCES = ../arena.c ../csv_config.c ../csv_utils.c ../csv_parser.c ../csv_writer.c ../csv_reader.c

.PHONY: all clean

all: benchmark_simd_on benchmark_simd_off

benchmark_simd_on: benchmark.c $(LIB_SOURCES)
	$(CC) -O3 -mavx2 $(CFLAGS) -o $@ benchmark.c $(LIB_SOURCES)

benchmark_simd_off: benchmark.c $(LIB_SOURCES)
	$(CC) -O3 -mno-sse2 -mno-avx2 $(CFLAGS) -o $@ benchmark.c $(LIB_SOURCES)

clean:
	rm -f benchmark_simd_on benchmark_simd_off

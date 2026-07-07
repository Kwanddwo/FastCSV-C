#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../csv_parser.h"
#include "../csv_reader.h"
#include "../csv_config.h"
#include "../arena.h"

#define TIMED_RUNS 10
#define ARENA_SIZE (4UL * 1024 * 1024)

typedef struct {
    double times[TIMED_RUNS];
    int count;
    double min;
    double max;
    double avg;
    double lines_per_sec;
} BenchmarkResult;

typedef struct {
    char *data;
    size_t *line_lens;
    size_t *offsets;
    int count;
} FileLines;

static double timespec_to_ms(struct timespec *ts) {
    return ts->tv_sec * 1000.0 + ts->tv_nsec / 1000000.0;
}

static FileLines load_lines(const char *filename, Arena *arena) {
    FileLines fl = {0};
    FILE *file = fopen(filename, "r");
    if (!file) return fl;

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buf = NULL;
    ArenaResult ar = arena_alloc(arena, file_size + 1, (void**)&buf);
    if (ar != ARENA_OK) { fclose(file); return fl; }
    size_t nread = fread(buf, 1, file_size, file);
    buf[nread] = '\0';

    int cap = 1024;
    fl.line_lens = NULL;
    ArenaResult ar2 = arena_alloc(arena, sizeof(size_t) * cap, (void**)&fl.line_lens);
    if (ar2 != ARENA_OK) { fclose(file); return fl; }

    size_t line_start = 0;
    for (size_t i = 0; i < nread; i++) {
        if (buf[i] == '\n') {
            if (fl.count >= cap) {
                cap *= 2;
                size_t *new_lens;
                ArenaResult ar3 = arena_alloc(arena, sizeof(size_t) * cap, (void**)&new_lens);
                if (ar3 != ARENA_OK) break;
                memcpy(new_lens, fl.line_lens, sizeof(size_t) * fl.count);
                fl.line_lens = new_lens;
            }
            size_t len = i - line_start;
            if (len > 0 && buf[i-1] == '\r') len--;
            fl.line_lens[fl.count++] = len;
            line_start = i + 1;
        }
    }
    if (line_start < nread) {
        if (fl.count >= cap) {
            cap *= 2;
            size_t *new_lens;
            ArenaResult ar3 = arena_alloc(arena, sizeof(size_t) * cap, (void**)&new_lens);
            if (ar3 != ARENA_OK) { fclose(file); return fl; }
            memcpy(new_lens, fl.line_lens, sizeof(size_t) * fl.count);
            fl.line_lens = new_lens;
        }
        fl.line_lens[fl.count++] = nread - line_start;
    }

    fl.offsets = NULL;
    ArenaResult ar4 = arena_alloc(arena, sizeof(size_t) * (fl.count + 1), (void**)&fl.offsets);
    if (ar4 != ARENA_OK) { fclose(file); return fl; }
    fl.offsets[0] = 0;
    for (int i = 0; i < fl.count; i++) {
        fl.offsets[i + 1] = fl.offsets[i] + fl.line_lens[i] + 1;
        buf[fl.offsets[i] + fl.line_lens[i]] = '\0';
    }

    fl.data = buf;
    fclose(file);
    return fl;
}

static inline char* get_line(FileLines *fl, int idx) {
    return fl->data + fl->offsets[idx];
}

static void preheat_cache(FileLines *fl) {
    volatile size_t sum = 0;
    for (int i = 0; i < fl->count; i++) {
        char *line = get_line(fl, i);
        size_t len = fl->line_lens[i];
        for (size_t j = 0; j < len; j++) sum += line[j];
    }
    (void)sum;
}

static BenchmarkResult run_benchmark(const char *filename, CSVConfig *config) {
    BenchmarkResult result = {0};
    Arena file_arena;
    arena_create(&file_arena, 16UL * 1024 * 1024);
    FileLines fl = load_lines(filename, &file_arena);
    if (!fl.data || fl.count == 0) {
        fprintf(stderr, "Error: cannot read %s\n", filename);
        arena_destroy(&file_arena);
        return result;
    }

    const int total_iterations = 1 + TIMED_RUNS;

    for (int i = 0; i < total_iterations; i++) {
        Arena parse_arena;
        arena_create(&parse_arena, ARENA_SIZE);

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);

        for (int ln = 0; ln < fl.count; ln++) {
            char *line = get_line(&fl, ln);
            csv_parse_line_inplace(line, &parse_arena, config, ln + 1);
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
        arena_destroy(&parse_arena);

        if (i > 0) {
            result.times[result.count++] = timespec_to_ms(&t_end) - timespec_to_ms(&t_start);
        } else {
            preheat_cache(&fl);
        }
    }

    arena_destroy(&file_arena);

    result.min = result.times[0];
    result.max = result.times[0];
    double sum = 0;
    for (int i = 0; i < result.count; i++) {
        if (result.times[i] < result.min) result.min = result.times[i];
        if (result.times[i] > result.max) result.max = result.times[i];
        sum += result.times[i];
    }
    result.avg = sum / result.count;
    result.lines_per_sec = fl.count / (result.min / 1000.0);

    return result;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <csv_file1> [csv_file2 ...]\n", argv[0]);
        return 1;
    }

    Arena arena;
    arena_create(&arena, ARENA_SIZE);
    CSVConfig *config = csv_config_create(&arena);
    csv_config_set_delimiter(config, ',');
    csv_config_set_enclosure(config, '"');

    for (int f = 1; f < argc; f++) {
        BenchmarkResult result = run_benchmark(argv[f], config);

        if (result.count == 0) {
            printf("%s: FAILED\n", argv[f]);
            continue;
        }

        const char *base = strrchr(argv[f], '/');
        if (!base) base = argv[f]; else base++;
        printf("%s:", base);
        for (int i = 0; i < result.count; i++) {
            printf(" %.3f", result.times[i]);
        }
        printf(" min=%.3f max=%.3f avg=%.3f\n",
               result.min, result.max, result.avg);
    }

    arena_destroy(&arena);
    return 0;
}

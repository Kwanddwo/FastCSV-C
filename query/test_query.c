#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arena.h"
#include "../csv_config.h"
#include "query.h"

int main(void) {
    Arena arena;
    ArenaResult ar = arena_create(&arena, 1024 * 1024);
    if (ar != ARENA_OK) {
        fprintf(stderr, "Failed to create arena\n");
        return 1;
    }

    CSVConfig *config = csv_config_create(&arena);
    csv_config_set_has_header(config, true);

    printf("=== Test 1: SELECT * ===\n");
    QueryResult r1 = query_execute(config, "SELECT * FROM 'students.csv';", &arena);
    if (r1.error) {
        printf("Error: %s (line %d, col %d)\n", r1.error, r1.error_line, r1.error_column);
    } else {
        printf("Headers: ");
        for (int i = 0; i < r1.header_count; i++) {
            printf("%s%s", i ? ", " : "", r1.headers[i]);
        }
        printf("\n");
        printf("Records: %d\n", r1.record_count);
        for (int i = 0; i < r1.record_count; i++) {
            printf("  [%d]: ", i);
            for (size_t j = 0; j < r1.records[i]->field_count; j++) {
                printf("%s%s", j ? ", " : "", r1.records[i]->fields[j]);
            }
            printf("\n");
        }
    }

    printf("\n=== Test 2: SELECT specific columns ===\n");
    QueryResult r2 = query_execute(config, "SELECT name, age FROM 'students.csv';", &arena);
    if (r2.error) {
        printf("Error: %s (line %d, col %d)\n", r2.error, r2.error_line, r2.error_column);
    } else {
        printf("Headers: ");
        for (int i = 0; i < r2.header_count; i++) {
            printf("%s%s", i ? ", " : "", r2.headers[i]);
        }
        printf("\n");
        printf("Records: %d\n", r2.record_count);
        for (int i = 0; i < r2.record_count; i++) {
            printf("  [%d]: ", i);
            for (size_t j = 0; j < r2.records[i]->field_count; j++) {
                printf("%s%s", j ? ", " : "", r2.records[i]->fields[j]);
            }
            printf("\n");
        }
    }

    printf("\n=== Test 3: Parse error ===\n");
    QueryResult r3 = query_execute(config, "SELECTX * FROM 'students.csv';", &arena);
    if (r3.error) {
        printf("Error: %s (line %d, col %d)\n", r3.error, r3.error_line, r3.error_column);
    }

    printf("\n=== Test 4: Column not found ===\n");
    QueryResult r4 = query_execute(config, "SELECT FakeCol FROM 'students.csv';", &arena);
    if (r4.error) {
        printf("Error: %s\n", r4.error);
    }

    arena_destroy(&arena);
    return 0;
}

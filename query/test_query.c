#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../arena.h"
#include "../csv_config.h"
#include "query.h"

/* Integration checks for query engine end-to-end behavior (real assertions,
   not eyeballing): pass-through projection, column selection, parse errors
   and semantic errors. Fixture: query/data/students.csv
   (name,age,city / Alice,20,NYC / Bob,22,LA / Charlie,19,Chicago /
   Diana,21,NYC / Eve,23,LA). */

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void check_field(QueryResult *r, int row, int col, const char *expect) {
    if (r->error != NULL || row >= r->record_count ||
        col >= (int)r->records[row]->field_count) {
        printf("FAIL: field check row %d col %d out of range\n", row, col);
        failures++;
        return;
    }
    CHECK(strcmp(r->records[row]->fields[col], expect) == 0, expect);
}

static void check_headers(QueryResult *r, const char **expect, int count) {
    CHECK(r->header_count == count, "header count");
    for (int i = 0; i < count && i < r->header_count; i++) {
        CHECK(strcmp(r->headers[i], expect[i]) == 0, "header name");
    }
}

int main(void) {
    Arena cfg_arena;
    ArenaResult ar = arena_create(&cfg_arena, 2 * 1024); /* matches csvql's CFG_ARENA_SIZE */
    if (ar != ARENA_OK) {
        fprintf(stderr, "Failed to create config arena\n");
        return 1;
    }

    CSVConfig *config = csv_config_create(&cfg_arena);
    csv_config_set_has_header(config, true);

    printf("=== Test 1: SELECT * round-tripped ===\n");
    QueryResult r1 = query_execute(config, "SELECT * FROM 'query/data/students.csv';");
    CHECK(r1.error == NULL, "SELECT * is valid");
    {
        const char *hdr[] = { "name", "age", "city" };
        check_headers(&r1, hdr, 3);
    }
    CHECK(r1.record_count == 5, "SELECT * row count");
    /* Pass-through fidelity: cells stay textually identical to the file. */
    check_field(&r1, 0, 0, "Alice");
    check_field(&r1, 0, 1, "20");
    check_field(&r1, 0, 2, "NYC");
    check_field(&r1, 2, 1, "19");
    check_field(&r1, 3, 2, "NYC");
    check_field(&r1, 4, 1, "23");
    check_field(&r1, 4, 2, "LA");
    query_result_destroy(&r1);

    printf("=== Test 2: SELECT specific columns ===\n");
    QueryResult r2 = query_execute(config, "SELECT name, age FROM 'query/data/students.csv';");
    CHECK(r2.error == NULL, "column selection is valid");
    {
        const char *hdr[] = { "name", "age" };
        check_headers(&r2, hdr, 2);
    }
    CHECK(r2.record_count == 5, "column selection row count");
    check_field(&r2, 0, 0, "Alice");
    check_field(&r2, 0, 1, "20");
    check_field(&r2, 4, 1, "23");
    CHECK(r2.records[0]->field_count == 2, "projected width");
    query_result_destroy(&r2);

    printf("=== Test 3: Parse error ===\n");
    QueryResult r3 = query_execute(config, "SELECTX * FROM 'query/data/students.csv';");
    CHECK(r3.error != NULL, "parse error reported");
    if (r3.error) CHECK(strstr(r3.error, "Expected 'SELECT'") != NULL, "parse error text");
    query_result_destroy(&r3);

    printf("=== Test 4: Column not found ===\n");
    QueryResult r4 = query_execute(config, "SELECT FakeCol FROM 'query/data/students.csv';");
    CHECK(r4.error != NULL, "semantic error reported");
    if (r4.error) {
        CHECK(strstr(r4.error, "Column") != NULL, "semantic error mentions column");
        CHECK(strstr(r4.error, "not found") != NULL, "semantic error mentions not found");
    }
    query_result_destroy(&r4);

    arena_destroy(&cfg_arena);

    if (failures == 0) {
        printf("\n✅ All query integration checks passed.\n");
        return 0;
    }
    printf("\n❌ %d check(s) failed.\n", failures);
    return 1;
}

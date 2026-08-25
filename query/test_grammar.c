/*
 * Comprehensive grammar tests for the entire SQL metasyntax.
 * Some tests document features that are not yet implemented and
 * are expected to fail.  This is by design.
 *
* Fixture note: table references resolve relative to the repo root,
 * so run via `make test-grammar` (or from the repository root).
 * The canonical fixtures live in query/data/ ('query/data/students.csv',
 * 'query/data/distinct.csv', 'query/data/nulls.csv', "query/data/my data.csv").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../arena.h"
#include "../csv_config.h"
#include "../csv_reader.h"
#include "query.h"

static int pass, fail;

/* ========== helpers ========== */
static QueryResult run_query(const char *sql, Arena *cfg_arena) {
    CSVConfig *cfg = csv_config_create(cfg_arena);
    csv_config_set_has_header(cfg, 1);
    return query_execute(cfg, sql, 0);
}

typedef enum {
    CHECK_ROWS,
    CHECK_ANY,
    CHECK_COLS,
    CHECK_ERROR,
    CHECK_PARSE_ERROR_COUNT,
    CHECK_VALUE,
} CheckKind;

/* Run one query and assert the expected property. Returns 1 on failure. */
static int test_sql(const char *sql, CheckKind kind, int expect, const char *expect_sub) {
    Arena cfg_arena;
    ArenaResult ar = arena_create(&cfg_arena, 2 * 1024); /* config only; the
        query engine sizes its own result arena via the estimator */
    if (ar != ARENA_OK) {
        printf("FAIL [arena]: %s\n", sql);
        fail++;
        return 1;
    }

    QueryResult res = run_query(sql, &cfg_arena);
    int ok = 0;

    switch (kind) {
        case CHECK_ROWS:
            if (res.error) {
                printf("FAIL: %s\n  -- unexpected error: %s\n", sql, res.error);
            } else if (res.record_count != expect) {
                printf("FAIL: %s\n  -- expected %d rows, got %d\n", sql, expect, res.record_count);
            } else {
                ok = 1;
            }
            break;
        case CHECK_ANY:
            if (res.error) {
                printf("FAIL: %s\n  -- unexpected error: %s\n", sql, res.error);
            } else {
                ok = 1;
            }
            break;
        case CHECK_COLS:
            if (res.error) {
                printf("FAIL: %s\n  -- unexpected error: %s\n", sql, res.error);
            } else if (res.header_count != expect) {
                printf("FAIL: %s\n  -- expected %d columns, got %d\n", sql, expect, res.header_count);
            } else {
                ok = 1;
            }
            break;
        case CHECK_ERROR:
            if (!res.error) {
                printf("FAIL: %s\n  -- expected error containing '%s', got success\n", sql, expect_sub);
            } else if (strstr(res.error, expect_sub) == NULL) {
                printf("FAIL: %s\n  -- expected error containing '%s', got '%s'\n", sql, expect_sub, res.error);
            } else {
                ok = 1;
            }
            break;
        case CHECK_PARSE_ERROR_COUNT: {
            int count = res.parse_errors ? res.parse_errors->count : 0;
            if (count != expect) {
                printf("FAIL: %s\n  -- expected %d parse error(s), got %d\n", sql, expect, count);
                for (int i = 0; i < count; i++) {
                    printf("     [%d] line %d, col %d: %s\n",
                           i, res.parse_errors->error_lines[i],
                           res.parse_errors->error_columns[i],
                           res.parse_errors->errors[i]);
                }
            } else {
                ok = 1;
            }
            break;
        }
        case CHECK_VALUE:
            if (res.error) {
                printf("FAIL: %s\n  -- unexpected error: %s\n", sql, res.error);
            } else if (res.record_count != 1 || res.records[0]->field_count == 0 ||
                       strcmp(res.records[0]->fields[0], expect_sub) != 0) {
                printf("FAIL: %s\n  -- expected first value '%s'\n", sql, expect_sub);
            } else {
                ok = 1;
            }
            break;
    }

    if (ok) pass++;
    else fail++;
    query_result_destroy(&res);
    arena_destroy(&cfg_arena);
    return ok ? 0 : 1;
}

static int test_query(const char *sql, int expect_rows) {
    return test_sql(sql, CHECK_ROWS, expect_rows, NULL);
}

static int test_query_any(const char *sql) {
    return test_sql(sql, CHECK_ANY, 0, NULL);
}

static int test_query_header_count(const char *sql, int expect_cols) {
    return test_sql(sql, CHECK_COLS, expect_cols, NULL);
}

static int test_query_error(const char *sql, const char *expect_sub) {
    return test_sql(sql, CHECK_ERROR, 0, expect_sub);
}

static int test_query_value(const char *sql, const char *expect_value) {
    return test_sql(sql, CHECK_VALUE, 0, expect_value);
}

/* =================================================================
 * 1. sql / statement – top level
 * ================================================================= */
static void test_top_level(void) {
    printf("--- sql / statement\n");
    test_query("SELECT * FROM 'query/data/students.csv'", 5);
    test_query_error("SELECT * FROM 'query/data/students.csv' EXTRA", "Unexpected token");
}

/* =================================================================
 * 2. select_query
 * ================================================================= */
static void test_select_query(void) {
    printf("--- select_query\n");
    test_query("SELECT * FROM 'query/data/students.csv'", 5);
    test_query("SELECT DISTINCT city FROM 'query/data/students.csv'", 3);
    /* ORDER BY */
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY name", 5);
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY name ASC", 5);
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY name DESC", 5);
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY age", 5);
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY age DESC, name ASC", 5);
    /* LIMIT / OFFSET */
    test_query("SELECT * FROM 'query/data/students.csv' LIMIT 1", 1);
    test_query("SELECT * FROM 'query/data/students.csv' LIMIT 3", 3);
    test_query("SELECT * FROM 'query/data/students.csv' LIMIT 10", 5);
    test_query("SELECT * FROM 'query/data/students.csv' OFFSET 2", 3);
    test_query("SELECT * FROM 'query/data/students.csv' LIMIT 2 OFFSET 1", 2);
    test_query("SELECT * FROM 'query/data/students.csv' OFFSET 1 LIMIT 2", 2);
    test_query("SELECT * FROM 'query/data/students.csv' LIMIT 0", 0);
    test_query("SELECT * FROM 'query/data/students.csv' OFFSET 10", 0);
    /* ORDER BY + LIMIT */
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY age DESC LIMIT 2", 2);
    /* GROUP BY / HAVING */
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING COUNT(*) > 1", 2);
    /* JOIN – parser will see JOIN as identifier then error */
    test_query_error("SELECT * FROM 'query/data/students.csv' JOIN 'x.csv'", "Unexpected token");
}

/* =================================================================
 * 3. select_list / select_item
 * ================================================================= */
static void test_select_list(void) {
    printf("--- select_list / select_item\n");
    test_query_header_count("SELECT * FROM 'query/data/students.csv'", 3);
    test_query_header_count("SELECT name, age FROM 'query/data/students.csv'", 2);
    test_query_any("SELECT age + 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age + 1 AS a FROM 'query/data/students.csv'");
    test_query_any("SELECT age + 1 a FROM 'query/data/students.csv'");
    test_query_header_count("SELECT name, age, city, age * 2, UPPER(name) FROM 'query/data/students.csv'", 5);
    /* scalar subquery in select_list – unimplemented */
    test_query_error("SELECT (SELECT age FROM 'query/data/students.csv') FROM 'query/data/students.csv'", "not supported");
}

/* =================================================================
 * 4. table_reference
 * ================================================================= */
static void test_table_reference(void) {
    printf("--- table_reference\n");
    test_query("SELECT * FROM 'query/data/students.csv'", 5);                               /* string literal */
    test_query_any("SELECT * FROM \"query/data/students\"");                            /* sans-extension (resolves to data/students.csv) */
    test_query_any("SELECT * FROM \"query/data/students.csv\"");                        /* double-quoted identifier */
    test_query_any("SELECT * FROM \"query/data/my data.csv\"");                             /* path with a space */
    test_query_error("SELECT * FROM (SELECT * FROM 'query/data/students.csv') AS t", "Expected"); /* subquery in FROM */
}

/* =================================================================
 * 5. alias
 * ================================================================= */
static void test_alias(void) {
    printf("--- alias\n");
    test_query_any("SELECT name AS n FROM 'query/data/students.csv'");
    test_query_any("SELECT name n FROM 'query/data/students.csv'");
    test_query_any("SELECT age * 2 AS double_age FROM 'query/data/students.csv'");
}

/* =================================================================
 * 6. expression – bitwise_expr
 * ================================================================= */
static void test_bitwise_expr(void) {
    printf("--- bitwise_expr (& | ^)\n");
    test_query_any("SELECT 6 & 3 FROM 'query/data/students.csv'");
    test_query_any("SELECT 6 | 3 FROM 'query/data/students.csv'");
    test_query_any("SELECT 6 ^ 3 FROM 'query/data/students.csv'");
    test_query_any("SELECT 6 & 3 | 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age & 3 FROM 'query/data/students.csv'");
    test_query_any("SELECT age | 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age ^ 2 FROM 'query/data/students.csv'");
}

/* =================================================================
 * 7. expression – additive_expr
 * ================================================================= */
static void test_additive_expr(void) {
    printf("--- additive_expr (+ -)\n");
    test_query_any("SELECT age + 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age - 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age + 1 + 1 FROM 'query/data/students.csv'");
    test_query_any("SELECT age + 1 - 2 FROM 'query/data/students.csv'");
}

/* =================================================================
 * 8. expression – multiplicative_expr
 * ================================================================= */
static void test_multiplicative_expr(void) {
    printf("--- multiplicative_expr (* / %%)\n");
    test_query_any("SELECT age * 2 FROM 'query/data/students.csv'");
    test_query_any("SELECT age / 2 FROM 'query/data/students.csv'");
    test_query_any("SELECT age % 3 FROM 'query/data/students.csv'");
    test_query_any("SELECT age * 2 / 4 FROM 'query/data/students.csv'");
}

/* =================================================================
 * 9. expression – operator precedence
 * ================================================================= */
static void test_precedence(void) {
    printf("--- expression precedence\n");
    test_query_any("SELECT 1 + 2 * 3 FROM 'query/data/students.csv'");        /* 7, not 9 */
    test_query_any("SELECT (1 + 2) * 3 FROM 'query/data/students.csv'");      /* 9 */
    test_query_any("SELECT 1 * 2 + 3 FROM 'query/data/students.csv'");        /* 5 */
    test_query_any("SELECT 1 + 2 * 3 - 4 / 2 FROM 'query/data/students.csv'"); /* 1+6-2 = 5 */
    test_query_any("SELECT 1 & 2 + 3 FROM 'query/data/students.csv'");        /* & after + : 1 & 5 = 1 */
}

/* =================================================================
 * 10. unary + / -
 * ================================================================= */
static void test_unary(void) {
    printf("--- unary + / -\n");
    test_query_any("SELECT -age FROM 'query/data/students.csv'");
    test_query_any("SELECT +age FROM 'query/data/students.csv'");
    /* Double negation needs the space: "--" now starts a comment */
    test_query_any("SELECT - -age FROM 'query/data/students.csv'");
    test_query_any("SELECT -age + 1 FROM 'query/data/students.csv'");
}

/* =================================================================
 * 11. function_call
 * ================================================================= */
static void test_function_calls(void) {
    printf("--- function_call\n");
    test_query_any("SELECT LENGTH('hello') FROM 'query/data/students.csv'");
    test_query_any("SELECT UPPER(name) FROM 'query/data/students.csv'");
    test_query_any("SELECT LOWER(city) FROM 'query/data/students.csv'");
    test_query_any("SELECT SUBSTR(name, 1, 3) FROM 'query/data/students.csv'");
    test_query_any("SELECT TRIM(city) FROM 'query/data/students.csv'");
    test_query_any("SELECT COALESCE(NULL, 'hi') FROM 'query/data/students.csv'");
    test_query_any("SELECT IFNULL(NULL, 'hi') FROM 'query/data/students.csv'");
    test_query_any("SELECT ABS(-5) FROM 'query/data/students.csv'");
    test_query_any("SELECT ROUND(3.14159, 2) FROM 'query/data/students.csv'");
    test_query_any("SELECT LENGTH(UPPER(name)) FROM 'query/data/students.csv'");
    test_query_any("SELECT CONCAT(name, ' ', city) FROM 'query/data/students.csv'");
    test_query_any("SELECT UCASE(name) FROM 'query/data/students.csv'");
    test_query_any("SELECT LCASE(city) FROM 'query/data/students.csv'");
    test_query_any("SELECT SUBSTRING(name, 2, 2) FROM 'query/data/students.csv'");
    /* aggregate functions */
    test_query("SELECT COUNT(*) FROM 'query/data/students.csv'", 1);
    test_query("SELECT SUM(age) FROM 'query/data/students.csv'", 1);
    test_query("SELECT AVG(age) FROM 'query/data/students.csv'", 1);
    test_query("SELECT MIN(age) FROM 'query/data/students.csv'", 1);
    test_query("SELECT MAX(age) FROM 'query/data/students.csv'", 1);
    test_query("SELECT COUNT(*) FROM 'query/data/students.csv' WHERE age > 20", 1);
    test_query("SELECT COUNT(name) FROM 'query/data/students.csv'", 1);
    test_query("SELECT MAX(name) FROM 'query/data/students.csv'", 1);
    test_query("SELECT ROUND(AVG(age), 1) FROM 'query/data/students.csv'", 1);
    test_query("SELECT MAX(age) + 1 FROM 'query/data/students.csv'", 1);
    test_query("SELECT SUM(age) FROM 'query/data/students.csv' WHERE age > 99", 1);      /* NULL -> 1 row */
    test_query("SELECT COUNT(*), MAX(age) FROM 'query/data/students.csv'", 1);
    test_query("SELECT 'Total:', COUNT(*) FROM 'query/data/students.csv'", 1);           /* literal + aggregate */
    test_query("SELECT COUNT(DISTINCT city) FROM 'query/data/students.csv'", 1);
    test_query("SELECT SUM(DISTINCT age) FROM 'query/data/students.csv'", 1);
    /* Value-aware DISTINCT on mixed data: '5' and '05' collapse to one value,
   so COUNT(DISTINCT val) is 7 and the numeric sum drops the duplicate. */
    test_query_value("SELECT COUNT(DISTINCT val) FROM 'query/data/distinct.csv'", "7");
    test_query_value("SELECT SUM(DISTINCT val) FROM 'query/data/distinct.csv'", "28.5");
    test_query_value("SELECT AVG(DISTINCT val) FROM 'query/data/distinct.csv'", "5.7");
    test_query("SELECT grp, COUNT(DISTINCT val) FROM 'query/data/distinct.csv' GROUP BY grp", 3);
    test_query("SELECT MAX(age) FROM 'query/data/students.csv' LIMIT 1", 1);
    test_query_error("SELECT name, MAX(age) FROM 'query/data/students.csv'", "GROUP BY");
    test_query_error("SELECT MAX(age) FROM 'query/data/students.csv' WHERE SUM(age) > 5", "not supported");
    test_query_error("SELECT COUNT(DISTINCT *) FROM 'query/data/students.csv'", "cannot be applied");
    test_query_error("SELECT UPPER(DISTINCT name) FROM 'query/data/students.csv'", "DISTINCT is only allowed");
    /* unknown function */
    test_query_error("SELECT nonexistent(name) FROM 'query/data/students.csv'", "Unknown function");
}

/* =================================================================
 * 12. group_by / having
 * ================================================================= */
static void test_group_by_having(void) {
    printf("--- group_by / having\n");
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT city, MAX(age) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT age, COUNT(*) FROM 'query/data/students.csv' GROUP BY age", 5);
    test_query("SELECT city FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT city, SUM(age) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT city, AVG(age) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT LOWER(city), COUNT(*) FROM 'query/data/students.csv' GROUP BY LOWER(city)", 3);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING COUNT(*) > 1", 2);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING COUNT(*) < 2", 1);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING city = 'LA'", 1);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city ORDER BY COUNT(*) DESC", 3);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city LIMIT 2", 2);
    test_query("SELECT city, COUNT(DISTINCT name) FROM 'query/data/students.csv' GROUP BY city", 3);
    test_query("SELECT COUNT(*) FROM 'query/data/students.csv' HAVING COUNT(*) > 1", 1);
    test_query("SELECT COUNT(*) FROM 'query/data/students.csv' WHERE age > 20 HAVING COUNT(*) > 1", 1);
    test_query("SELECT COUNT(*) FROM 'query/data/students.csv' WHERE age > 99 HAVING COUNT(*) > 1", 0);
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' WHERE age > 20 GROUP BY city", 2);
    /* validation errors */
    test_query_error("SELECT name FROM 'query/data/students.csv' GROUP BY city", "GROUP BY");
    test_query_error("SELECT * FROM 'query/data/students.csv' GROUP BY city", "GROUP BY");
    test_query_error("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING name = 'x'", "GROUP BY");
    test_query_error("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city ORDER BY name", "GROUP BY");
    test_query_error("SELECT * FROM 'query/data/students.csv' HAVING age > 0", "HAVING without GROUP BY");
}

/* =================================================================
 * 13. case_expression
 * ================================================================= */
static void test_case_expression(void) {
    printf("--- case_expression\n");
    /* searched CASE */
    test_query_any("SELECT CASE WHEN 1 < 2 THEN 'yes' END FROM 'query/data/students.csv'");
    test_query_any(
        "SELECT CASE WHEN age < 21 THEN 'young' WHEN age < 23 THEN 'middle' ELSE 'old' END "
        "FROM 'query/data/students.csv'");
    /* simple CASE */
    test_query_any(
        "SELECT CASE city WHEN 'NYC' THEN 'NY' WHEN 'LA' THEN 'CA' ELSE city END "
        "FROM 'query/data/students.csv'");
    /* CASE without ELSE – yields NULL */
    test_query_any("SELECT CASE WHEN age < 20 THEN 'young' END FROM 'query/data/students.csv'");
    /* CASE inside expression */
    test_query_any("SELECT 1 + CASE WHEN 1 < 2 THEN 3 ELSE 4 END FROM 'query/data/students.csv'");
}

/* =================================================================
 * 14. qualified_identifier
 * ================================================================= */
static void test_qualified_identifier(void) {
    printf("--- qualified_identifier\n");
    test_query_any("SELECT students.name FROM 'query/data/students.csv'");
    test_query_any("SELECT * FROM 'query/data/students.csv' WHERE students.age > 20");
}

/* =================================================================
 * 15. double-quoted identifiers
 * ================================================================= */
static void test_quoted_identifier(void) {
    printf("--- quoted identifier\n");
    test_query_any("SELECT \"name\" FROM 'query/data/students.csv'");
    test_query_any("SELECT \"first name\" FROM \"query/data/my data.csv\"");
    test_query_any("SELECT \"select\" FROM \"query/data/my data.csv\"");        /* keyword as identifier */
    test_query_any("SELECT students.\"name\" FROM 'query/data/students.csv'"); /* qualified, quoted column */
    test_query_any("SELECT \"name\" AS \"display name\" FROM 'query/data/students.csv'");
    test_query_any("SELECT \"name\" FROM 'query/data/students.csv' WHERE \"name\" = 'Alice'");
    test_query_any("SELECT \"name\" FROM 'query/data/students.csv' ORDER BY \"name\"");
    test_query_any("SELECT \"name\" FROM 'query/data/students.csv' GROUP BY \"name\"");
    test_query_error("SELECT \"unterminated FROM 'query/data/students.csv'", "Unterminated identifier");
}

/* =================================================================
 * 16. literal
 * ================================================================= */
static void test_literals(void) {
    printf("--- literal\n");
    test_query_any("SELECT 42 FROM 'query/data/students.csv'");
    test_query_any("SELECT 3.14 FROM 'query/data/students.csv'");
    test_query_any("SELECT 'hello' FROM 'query/data/students.csv'");
    test_query_any("SELECT 'it''s a test' FROM 'query/data/students.csv'");
    test_query_error("SELECT 'unterminated", "Unterminated string");
    test_query_any("SELECT NULL FROM 'query/data/students.csv'");
    test_query_any("SELECT TRUE FROM 'query/data/students.csv'");
    test_query_any("SELECT FALSE FROM 'query/data/students.csv'");
}

/* =================================================================
 * 15.5 constant folding
 * ================================================================= */
static void test_constant_folding(void) {
    printf("--- constant folding\n");
    test_query_value("SELECT 1 + 2 FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT 6 & 3 FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_value("SELECT 10 / 4 FROM 'query/data/students.csv' LIMIT 1", "2.5");
    test_query_value("SELECT UPPER('abc') FROM 'query/data/students.csv' LIMIT 1", "ABC");
    test_query_value("SELECT CASE WHEN 1 < 2 THEN 'yes' ELSE 'no' END FROM 'query/data/students.csv' LIMIT 1", "yes");
    /* Folded constants in WHERE keep their truthiness semantics */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE 1 = 1", 5);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE age > (1 + 2)", 5);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name LIKE 'A%' AND (1 = 1)", 1);
    /* A constant subtree that fails to evaluate stays unfolded and still
       errors per row (guard: folding must not swallow the error). */
    test_query_error("SELECT nonexistent(1) FROM 'query/data/students.csv'", "Unknown function");
}

/* =================================================================
 * 16. '(' expression ')'
 * ================================================================= */
static void test_parenthesized_expr(void) {
    printf("--- parenthesized expression\n");
    test_query_any("SELECT (age) FROM 'query/data/students.csv'");
    test_query_any("SELECT ((age)) FROM 'query/data/students.csv'");
    test_query_any("SELECT (age + 1) * 2 FROM 'query/data/students.csv'");
}

/* =================================================================
 * 17. '(' select_query ')' — scalar subquery
 * ================================================================= */
static void test_scalar_subquery(void) {
    printf("--- scalar subquery (unimplemented)\n");
    test_query_error("SELECT (SELECT age FROM 'query/data/students.csv') FROM 'query/data/students.csv'", "not supported");
}

/* =================================================================
 * 18. search_condition – NOT
 * ================================================================= */
static void test_search_not(void) {
    printf("--- NOT\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE NOT age = 20", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE NOT (age = 20)", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE NOT age < 21 AND city = 'LA'", 2);
}

/* =================================================================
 * 19. comparison_operator
 * ================================================================= */
static void test_comparison_operators(void) {
    printf("--- comparison_operator\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age = 20", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age != 20", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age <> 20", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age < 21", 2);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age > 21", 2);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age <= 21", 3);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age >= 21", 3);
    /* expressions on both sides */
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age + 1 = 21", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age = 10 + 10", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age + 1 = age + 1", 5);
}

/* =================================================================
 * 20. IN / NOT IN
 * ================================================================= */
static void test_search_in(void) {
    printf("--- IN / NOT IN\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE city IN ('NYC', 'LA')", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE city NOT IN ('NYC', 'LA')", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age IN (20, 22)", 2);
    /* IN (subquery) – unimplemented; should error */
    test_query_error("SELECT * FROM 'query/data/students.csv' WHERE age IN (SELECT 20 FROM 'query/data/students.csv')", "not supported");
}

/* =================================================================
 * 21. BETWEEN
 * ================================================================= */
static void test_search_between(void) {
    printf("--- BETWEEN\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age BETWEEN 20 AND 22", 3);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age BETWEEN 20 AND 22 AND city = 'NYC'", 2);
}

/* =================================================================
 * 22. LIKE / ILIKE
 * ================================================================= */
static void test_search_like(void) {
    printf("--- LIKE / ILIKE\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE name LIKE 'A%'", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE name LIKE '%e'", 3);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE name LIKE '%li%'", 2);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE name LIKE 'A____'", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE city ILIKE 'nyc'", 2);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE city ILIKE 'l%'", 2);
}

/* =================================================================
 * 23. AND / OR / precedence
 * ================================================================= */
static void test_search_and_or(void) {
    printf("--- AND / OR\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age > 20 AND city = 'NYC'", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age = 20 OR age = 22", 2);
    /* AND binds tighter than OR — 3 rows: (age>20 AND city=NYC)=Diana, OR city=LA=Bob+Eve => total 3 */
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age > 20 AND city = 'NYC' OR city = 'LA'", 3);
    /* explicit parens – same result */
    test_query("SELECT * FROM 'query/data/students.csv' WHERE (age > 20 AND city = 'NYC') OR city = 'LA'", 3);
    /* different grouping – age>20 AND (city=NYC OR city=LA) => Bob(22,LA), Diana(21,NYC), Eve(23,LA) = 3 */
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age > 20 AND (city = 'NYC' OR city = 'LA')", 3);
    /* chained AND */
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age > 19 AND age < 22 AND city = 'NYC'", 2);
    /* Three-valued logic: UNKNOWN ∧ TRUE = UNKNOWN, UNKNOWN ∨ TRUE = TRUE */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE (age > 10 OR NULL) AND name = 'Alice'", 1);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE (age > 10 AND NULL) OR name = 'Alice'", 1);
}

/* =================================================================
 * 23.5 NULL / three-valued logic / IS NULL
 * ================================================================= */
static void test_null_3vl(void) {
    printf("--- NULL / three-valued logic / IS NULL\n");

    /* IS NULL / IS NOT NULL (empty CSV cells evaluate to NULL) */
    test_query("SELECT name FROM 'query/data/nulls.csv' WHERE name IS NULL", 1);
    test_query("SELECT name FROM 'query/data/nulls.csv' WHERE name IS NOT NULL", 2);
    test_query("SELECT id FROM 'query/data/nulls.csv' WHERE note IS NULL", 1);
    test_query_value("SELECT COUNT(name) FROM 'query/data/nulls.csv'", "2");

    /* Comparison with NULL yields UNKNOWN, so WHERE rejects the row... */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name = NULL", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name <> NULL", 0);
    /* ...and NOT UNKNOWN stays UNKNOWN (previously flipped to TRUE) */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NOT (name = NULL)", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NOT (name <> NULL)", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NOT (age BETWEEN NULL AND 30)", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NOT (name LIKE NULL)", 0);

    /* IN with a NULL element: a match is TRUE; no match is UNKNOWN */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name IN ('Alice', NULL)", 1);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name NOT IN ('Alice', NULL)", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE name IN (NULL, 'Bob', NULL)", 1);

    /* NULL as a bare predicate, and NOT NULL, are UNKNOWN -> rejected */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NULL", 0);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE NOT NULL", 0);

    /* CASE WHEN: UNKNOWN conditions never match a WHEN */
    test_query_value("SELECT CASE WHEN NULL THEN 1 ELSE 2 END FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_value("SELECT CASE WHEN (1 = NULL) THEN 1 ELSE 2 END FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_value("SELECT CASE WHEN NULL AND FALSE THEN 1 WHEN NULL OR TRUE THEN 2 ELSE 3 END FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_value("SELECT CASE WHEN NULL AND TRUE THEN 1 ELSE 2 END FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_value("SELECT CASE WHEN NULL OR FALSE THEN 1 ELSE 2 END FROM 'query/data/students.csv' LIMIT 1", "2");
}

/* =================================================================
 * 23.75 ORDER BY ordinal / result-column references
 * ================================================================= */
static void test_order_by_references(void) {
    printf("--- ORDER BY ordinal / result-column references\n");
    test_query("SELECT name FROM 'query/data/students.csv' ORDER BY 1", 5);
    test_query("SELECT name FROM 'query/data/students.csv' ORDER BY 1 DESC", 5);
    test_query("SELECT name, age FROM 'query/data/students.csv' ORDER BY 2 DESC", 5);
    test_query("SELECT name AS n FROM 'query/data/students.csv' ORDER BY n", 5);
    test_query("SELECT name, age AS a FROM 'query/data/students.csv' ORDER BY a", 5);
    test_query("SELECT city AS c, COUNT(*) FROM 'query/data/students.csv' GROUP BY city ORDER BY c", 3);
    test_query("SELECT city AS c, COUNT(*) AS cnt FROM 'query/data/students.csv' GROUP BY city ORDER BY cnt DESC", 3);
    /* out-of-range ordinal is an error, not a constant sort */
    test_query_error("SELECT name FROM 'query/data/students.csv' ORDER BY 2", "not in the select list");
    test_query_error("SELECT name FROM 'query/data/students.csv' ORDER BY 0", "not in the select list");
    test_query_error("SELECT name FROM 'query/data/students.csv' ORDER BY 99", "not in the select list");
    /* ordering correctness, not just row counts */
    test_query_value("SELECT name FROM 'query/data/students.csv' ORDER BY 1 DESC LIMIT 1", "Eve");
    test_query_value("SELECT name, age FROM 'query/data/students.csv' ORDER BY 2 DESC LIMIT 1", "Eve");
    test_query_value("SELECT age FROM 'query/data/students.csv' ORDER BY 1 DESC LIMIT 1", "23");
    /* alias resolution precedence: the matching display name wins */
    test_query_value("SELECT name AS n, age AS name FROM 'query/data/students.csv' ORDER BY name LIMIT 1", "Charlie");
    /* resolving to a computed select item evaluates the item expression */
    test_query_value("SELECT age + 1 AS x FROM 'query/data/students.csv' ORDER BY x LIMIT 1", "20");
    /* DISTINCT combined with alias/ordinal references */
    test_query("SELECT DISTINCT city AS c FROM 'query/data/students.csv' ORDER BY c", 3);
    test_query("SELECT DISTINCT city FROM 'query/data/students.csv' ORDER BY 1", 3);
    /* aggregate ordinal over grouped results */
    test_query("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY city HAVING COUNT(*) > 1 ORDER BY 2 DESC", 2);
    /* a non-whole number stays a constant key (no error, no ordering) */
    test_query("SELECT name FROM 'query/data/students.csv' ORDER BY 1.5", 5);
    /* a valid position holding '*' is a constant key: runs, order unspecified */
    test_query("SELECT * FROM 'query/data/students.csv' ORDER BY 1", 5);
    /* GROUP BY aliases are deliberately not resolved (non-standard extension) */
    test_query_error("SELECT city AS c, COUNT(*) FROM 'query/data/students.csv' GROUP BY c", "Column 'c' not found");
    test_query_error("SELECT city, COUNT(*) FROM 'query/data/students.csv' GROUP BY 1", "must appear in the GROUP BY");
}

/* =================================================================
 * 23.9 NULLS FIRST / LAST
 * ================================================================= */
static void test_nulls_first_last(void) {
    printf("--- NULLS FIRST / LAST\n");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name NULLS LAST LIMIT 1", "Alice");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name NULLS FIRST LIMIT 1", "NULL");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name DESC NULLS FIRST LIMIT 1", "NULL");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name DESC NULLS LAST LIMIT 1", "Bob");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name DESC LIMIT 1", "Bob");   /* default: NULLs last on DESC */
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY name LIMIT 1", "NULL");      /* default: NULLs first on ASC */
    test_query_error("SELECT name FROM 'query/data/nulls.csv' ORDER BY name NULLS", "FIRST");
    test_query("SELECT name FROM 'query/data/nulls.csv' ORDER BY name NULLS LAST", 3);
    test_query("SELECT name FROM 'query/data/nulls.csv' ORDER BY name NULLS FIRST", 3);
    /* multi-key: NULL placement applies per key, remaining keys tie-break */
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY note NULLS FIRST, name LIMIT 1", "1");
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY note NULLS LAST, name LIMIT 1", "3");
    /* NULL placement through the top-k heap path (LIMIT window) */
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY note NULLS FIRST LIMIT 1", "1");
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY note NULLS LAST LIMIT 1", "3");
    /* numeric keys through the top-k heap path */
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY id DESC LIMIT 1", "3");
    test_query_value("SELECT id FROM 'query/data/nulls.csv' ORDER BY id LIMIT 1", "1");
    /* NULLS combined with an ordinal reference */
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY 1 NULLS LAST LIMIT 1", "Alice");
    test_query_value("SELECT name FROM 'query/data/nulls.csv' ORDER BY 1 NULLS FIRST LIMIT 1", "NULL");
}

/* =================================================================
 * 23.95 standard numeric / string functions
 * ================================================================= */
static void test_standard_functions(void) {
    printf("--- standard functions\n");
    test_query_value("SELECT FLOOR(3.7) FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT CEIL(3.2) FROM 'query/data/students.csv' LIMIT 1", "4");
    test_query_value("SELECT CEILING(3.2) FROM 'query/data/students.csv' LIMIT 1", "4");
    test_query_value("SELECT SQRT(16) FROM 'query/data/students.csv' LIMIT 1", "4");
    test_query_value("SELECT POWER(2, 10) FROM 'query/data/students.csv' LIMIT 1", "1024");
    test_query_value("SELECT MOD(10, 3) FROM 'query/data/students.csv' LIMIT 1", "1");
    test_query_value("SELECT SIGN(-5) FROM 'query/data/students.csv' LIMIT 1", "-1");
    test_query_value("SELECT SIGN(0) FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_value("SELECT CHAR_LENGTH('hello') FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT CHARACTER_LENGTH('hello') FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT POSITION('ll' IN 'hello') FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT POSITION('x' IN 'hello') FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_any("SELECT RANDOM() FROM 'query/data/students.csv' LIMIT 1");
    test_query_any("SELECT EXP(1), LN(2.7), LOG10(100), PI() FROM 'query/data/students.csv' LIMIT 1");
    /* string functions coerce numeric arguments to their text form */
    test_query_value("SELECT LENGTH(5) FROM 'query/data/students.csv' LIMIT 1", "1");
    test_query_value("SELECT UPPER(5) FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT TRIM(5) FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT SUBSTR(5, 1, 1) FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT POSITION(1 IN 'abc') FROM 'query/data/students.csv' LIMIT 1", "0");
    /* NULL propagates through function arguments */
    test_query_value("SELECT LENGTH(NULL) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT UPPER(NULL) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT POSITION('a' IN NULL) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT FLOOR(NULL) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    /* non-numeric / out-of-domain numeric arguments yield NULL */
    test_query_value("SELECT FLOOR('abc') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT MOD(NULL, 3) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT MOD(5, 0) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT SQRT(-1) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT LN(0) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT LOG10(0) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    /* boundary and sign values */
    test_query_value("SELECT SIGN(5) FROM 'query/data/students.csv' LIMIT 1", "1");
    test_query_value("SELECT FLOOR(-3.7) FROM 'query/data/students.csv' LIMIT 1", "-4");
    test_query_value("SELECT CEIL(-3.7) FROM 'query/data/students.csv' LIMIT 1", "-3");
    test_query_value("SELECT FLOOR(3.0) FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT CEIL(3.0) FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT SQRT(0) FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_value("SELECT POWER(2, -2) FROM 'query/data/students.csv' LIMIT 1", "0.25");
    test_query_value("SELECT MOD(7.5, 2) FROM 'query/data/students.csv' LIMIT 1", "1.5");
    test_query_value("SELECT POSITION('' IN 'abc') FROM 'query/data/students.csv' LIMIT 1", "1");
}

/* =================================================================
 * 24. date functions (ISO strings; extensions flagged)
 * ================================================================= */
static void test_date_functions(void) {
    printf("--- date functions\n");
    test_query_value("SELECT EXTRACT(YEAR FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "2024");
    test_query_value("SELECT EXTRACT(MONTH FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT EXTRACT(DAY FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "15");
    test_query_value("SELECT EXTRACT(HOUR FROM '2024-05-15 10:30:00') FROM 'query/data/students.csv' LIMIT 1", "10");
    test_query_value("SELECT EXTRACT(SECOND FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_value("SELECT EXTRACT(QUARTER FROM '2024-08-15') FROM 'query/data/students.csv' LIMIT 1", "3");
    test_query_value("SELECT EXTRACT(YEAR FROM 'not a date') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT DATE '2024-05-15' FROM 'query/data/students.csv' LIMIT 1", "2024-05-15");
    test_query_value("SELECT TIMESTAMP '2024-05-15 10:30:00' FROM 'query/data/students.csv' LIMIT 1", "2024-05-15 10:30:00");
    test_query_error("SELECT DATE 'bogus' FROM 'query/data/students.csv'", "Invalid date/time literal");
    test_query_error("SELECT EXTRACT(FORTNIGHT FROM '2024-05-15') FROM 'query/data/students.csv'", "Unknown EXTRACT field");
    /* extension forms (documented, not ISO standard) */
    test_query_value("SELECT YEAR('2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "2024");
    test_query_value("SELECT MONTH('2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT DAY('2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "15");
    test_query_value("SELECT DATEDIFF('2024-05-20', '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "5");
    test_query_value("SELECT DATEDIFF('2025-01-01', '2024-12-31') FROM 'query/data/students.csv' LIMIT 1", "1");
    test_query_value("SELECT DATEDIFF('2024-03-01', '2024-02-28') FROM 'query/data/students.csv' LIMIT 1", "2");
    test_query_any("SELECT CURRENT_DATE, CURRENT_TIME, CURRENT_TIMESTAMP, LOCALTIME, LOCALTIMESTAMP, NOW() FROM 'query/data/students.csv' LIMIT 1");
    /* dates as strings compare chronologically */
    test_query("SELECT name FROM 'query/data/students.csv' WHERE '2024-05-15' > '2024-01-01'", 5);
    test_query("SELECT name FROM 'query/data/students.csv' WHERE '2024-05-15' < '2024-01-01'", 0);
    /* EXTRACT edges: time fields on date-only strings, NULL/non-string values */
    test_query_value("SELECT EXTRACT(MINUTE FROM '2024-05-15 10:30:00') FROM 'query/data/students.csv' LIMIT 1", "30");
    test_query_value("SELECT EXTRACT(HOUR FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_value("SELECT EXTRACT(YEAR FROM NULL) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT EXTRACT(YEAR FROM 5) FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT EXTRACT(year FROM '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "2024");
    /* date-range validation */
    test_query_value("SELECT EXTRACT(DAY FROM '2024-02-29') FROM 'query/data/students.csv' LIMIT 1", "29");
    test_query_value("SELECT EXTRACT(DAY FROM '2023-02-29') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT EXTRACT(DAY FROM '2024-13-01') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT EXTRACT(DAY FROM '2024-05-15 25:00:00') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    /* TIME literal + malformed literals are parse errors */
    test_query_value("SELECT TIME '12:30:00' FROM 'query/data/students.csv' LIMIT 1", "12:30:00");
    test_query_error("SELECT TIME 'bogus' FROM 'query/data/students.csv'", "Invalid date/time literal");
    test_query_error("SELECT TIMESTAMP 'bogus' FROM 'query/data/students.csv'", "Invalid date/time literal");
    /* DATEDIFF edges */
    test_query_value("SELECT DATEDIFF('2024-05-15', '2024-05-20') FROM 'query/data/students.csv' LIMIT 1", "-5");
    test_query_value("SELECT DATEDIFF('2024-05-15', '2024-05-15') FROM 'query/data/students.csv' LIMIT 1", "0");
    test_query_value("SELECT DATEDIFF('x', '2024-01-01') FROM 'query/data/students.csv' LIMIT 1", "NULL");
    test_query_value("SELECT YEAR('not a date') FROM 'query/data/students.csv' LIMIT 1", "NULL");
}

/* =================================================================
 * 24. '(' search_condition ')'
 * ================================================================= */
static void test_paren_search_condition(void) {
    printf("--- parenthesized search condition\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE (age = 20)", 1);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE ((age = 20))", 1);
}

/* =================================================================
 * 25. bare expression (truthy evaluation)
 * ================================================================= */
static void test_bare_expression_condition(void) {
    printf("--- bare expression condition (truthy)\n");
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age", 5);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE age - 20", 4);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE name", 5);
}

/* =================================================================
 * 26. set_op (UNIMPLEMENTED)
 * ================================================================= */
static void test_set_op(void) {
    printf("--- set_op (unimplemented)\n");
    test_query_error("SELECT name FROM 'query/data/students.csv' UNION SELECT name FROM 'query/data/students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'query/data/students.csv' UNION ALL SELECT name FROM 'query/data/students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'query/data/students.csv' INTERSECT SELECT name FROM 'query/data/students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'query/data/students.csv' EXCEPT SELECT name FROM 'query/data/students.csv'",
                     "Unexpected token");
}

/* =================================================================
 * 27. Other statements (UNIMPLEMENTED)
 * ================================================================= */
static void test_other_statements(void) {
    printf("--- other statements (unimplemented)\n");
    test_query_error("INSERT INTO 'query/data/students.csv' VALUES ('x', 1, 'y')", "Expected 'SELECT'");
    test_query_error("UPDATE 'query/data/students.csv' SET age = 21", "Expected 'SELECT'");
    test_query_error("DELETE FROM 'query/data/students.csv'", "Expected 'SELECT'");
    test_query_error("CREATE TABLE foo (a INT)", "Expected 'SELECT'");
    test_query_error("ALTER TABLE foo ADD COLUMN b INT", "Expected 'SELECT'");
}

/* =================================================================
 * 28. Error recovery & multi-error collection
 * ================================================================= */
static int test_query_parse_error_count(const char *sql, int expect_count) {
    return test_sql(sql, CHECK_PARSE_ERROR_COUNT, expect_count, NULL);
}

static void test_error_recovery(void) {
    printf("--- error recovery / multi-error collection\n");

    /* Multiple missing commas in the select list -> multiple errors.
     * (Cascading errors at the same position are suppressed, so the
     * trailing 'Expected ...' duplicates do not inflate the count.) */
    test_query_parse_error_count("SELECT name age city FROM 'query/data/students.csv'", 2);
    test_query_parse_error_count("SELECT 1 2 3 FROM 'query/data/students.csv'", 2);

    /* Valid query has zero parse errors */
    test_query_parse_error_count("SELECT * FROM 'query/data/students.csv'", 0);

    /* Error limit is capped at 50 */
    test_query_parse_error_count("SELECT ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,, FROM 'query/data/students.csv'", 50);

    /* Runtime (semantic) errors still fail-fast and are not parse errors */
    test_query_error("SELECT FakeCol FROM 'query/data/students.csv'", "Column");
}

static void test_comments(void) {
    printf("--- SQL comments\n");

    /* Line comments: trailing, leading, and between tokens */
    test_query("SELECT * FROM 'query/data/students.csv' -- trailing note", 5);
    test_query("-- leading note\nSELECT * FROM 'query/data/students.csv'", 5);
    test_query("SELECT -- between tokens\n* FROM 'query/data/students.csv'", 5);
    test_query("SELECT * FROM 'query/data/students.csv' -- ORDER BY nothing", 5);

    /* Block comments: inline, multi-line, and inside WHERE */
    test_query("SELECT /* x */ * FROM 'query/data/students.csv'", 5);
    test_query("SELECT /* a\nb */ * FROM 'query/data/students.csv'", 5);
    test_query("SELECT * FROM 'query/data/students.csv' /* ; */", 5);
    test_query("SELECT * FROM 'query/data/students.csv' WHERE city = 'NYC' /* or LA */", 2);

    /* Apostrophes and keywords inside comments must be ignored */
    test_query("SELECT * FROM 'query/data/students.csv' -- it's fine", 5);
    test_query("SELECT * FROM 'query/data/students.csv' -- CONCAT(DISTINCT LIMIT 0", 5);

    /* Comment markers inside string literals are not comments */
    test_query("SELECT 'a -- b' FROM 'query/data/students.csv'", 5);
    test_query("SELECT 'a /* b' FROM 'query/data/students.csv'", 5);

    /* A lone '-' or '/' keeps its operator meaning */
    test_query_value("SELECT 5 - -3 FROM 'query/data/students.csv' LIMIT 1", "8");
    test_query_any("SELECT 10 / 2 FROM 'query/data/students.csv'");

    /* An unterminated block comment is a syntax error */
    test_query_error("SELECT * /* note FROM 'query/data/students.csv'", "Unterminated comment");
    test_query_parse_error_count("SELECT * /* note FROM 'query/data/students.csv'", 1);
}

/* =================================================================
 * main
 * ================================================================= */
int main(void) {
    printf("=== Comprehensive Grammar Coverage Tests ===\n\n");
    printf("Note: tests for unimplemented features (subqueries, set ops,\n"
           "DML/DDL) are EXPECTED to fail.\n"
           "They document the grammar and will pass once implemented.\n\n");

    test_top_level();
    test_select_query();
    test_select_list();
    test_table_reference();
    test_alias();
    test_bitwise_expr();
    test_additive_expr();
    test_multiplicative_expr();
    test_precedence();
    test_unary();
    test_function_calls();
    test_group_by_having();
    test_case_expression();
    test_qualified_identifier();
    test_quoted_identifier();
    test_literals();
    test_constant_folding();
    test_parenthesized_expr();
    test_scalar_subquery();
    test_search_not();
    test_comparison_operators();
    test_search_in();
    test_search_between();
    test_search_like();
    test_search_and_or();
    test_null_3vl();
    test_order_by_references();
    test_nulls_first_last();
    test_standard_functions();
    test_date_functions();
    test_paren_search_condition();
    test_bare_expression_condition();
    test_set_op();
    test_other_statements();
    test_error_recovery();
    test_comments();

    printf("\n=== Results ===\n");
    printf("PASS: %d   FAIL: %d   TOTAL: %d\n", pass, fail, pass + fail);

    return fail > 0 ? 1 : 0;
}

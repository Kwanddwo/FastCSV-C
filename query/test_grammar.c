/*
 * Comprehensive grammar tests for the entire SQL metasyntax.
 * Some tests document features that are not yet implemented and
 * are expected to fail.  This is by design.
 *
 * Fixture note: table references resolve relative to the repo root,
 * so run via `make test-grammar` (or from the repository root).
 * The canonical fixture is 'students.csv'.
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
static QueryResult run_query(const char *sql, Arena *arena) {
    CSVConfig *cfg = csv_config_create(arena);
    csv_config_set_has_header(cfg, 1);
    return query_execute(cfg, sql, arena);
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
    Arena arena;
    ArenaResult ar = arena_create(&arena, 512 * 1024);
    if (ar != ARENA_OK) {
        printf("FAIL [arena]: %s\n", sql);
        fail++;
        return 1;
    }

    QueryResult res = run_query(sql, &arena);
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
    arena_destroy(&arena);
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
    test_query("SELECT * FROM 'students.csv'", 5);
    test_query_error("SELECT * FROM 'students.csv' EXTRA", "Unexpected token");
}

/* =================================================================
 * 2. select_query
 * ================================================================= */
static void test_select_query(void) {
    printf("--- select_query\n");
    test_query("SELECT * FROM 'students.csv'", 5);
    test_query("SELECT DISTINCT city FROM 'students.csv'", 3);
    /* ORDER BY */
    test_query("SELECT * FROM 'students.csv' ORDER BY name", 5);
    test_query("SELECT * FROM 'students.csv' ORDER BY name ASC", 5);
    test_query("SELECT * FROM 'students.csv' ORDER BY name DESC", 5);
    test_query("SELECT * FROM 'students.csv' ORDER BY age", 5);
    test_query("SELECT * FROM 'students.csv' ORDER BY age DESC, name ASC", 5);
    /* LIMIT / OFFSET */
    test_query("SELECT * FROM 'students.csv' LIMIT 1", 1);
    test_query("SELECT * FROM 'students.csv' LIMIT 3", 3);
    test_query("SELECT * FROM 'students.csv' LIMIT 10", 5);
    test_query("SELECT * FROM 'students.csv' OFFSET 2", 3);
    test_query("SELECT * FROM 'students.csv' LIMIT 2 OFFSET 1", 2);
    test_query("SELECT * FROM 'students.csv' OFFSET 1 LIMIT 2", 2);
    test_query("SELECT * FROM 'students.csv' LIMIT 0", 0);
    test_query("SELECT * FROM 'students.csv' OFFSET 10", 0);
    /* ORDER BY + LIMIT */
    test_query("SELECT * FROM 'students.csv' ORDER BY age DESC LIMIT 2", 2);
    /* GROUP BY / HAVING */
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city HAVING COUNT(*) > 1", 2);
    /* JOIN – parser will see JOIN as identifier then error */
    test_query_error("SELECT * FROM 'students.csv' JOIN 'x.csv'", "Unexpected token");
}

/* =================================================================
 * 3. select_list / select_item
 * ================================================================= */
static void test_select_list(void) {
    printf("--- select_list / select_item\n");
    test_query_header_count("SELECT * FROM 'students.csv'", 3);
    test_query_header_count("SELECT name, age FROM 'students.csv'", 2);
    test_query_any("SELECT age + 1 FROM 'students.csv'");
    test_query_any("SELECT age + 1 AS a FROM 'students.csv'");
    test_query_any("SELECT age + 1 a FROM 'students.csv'");
    test_query_header_count("SELECT name, age, city, age * 2, UPPER(name) FROM 'students.csv'", 5);
    /* scalar subquery in select_list – unimplemented */
    test_query_error("SELECT (SELECT age FROM 'students.csv') FROM 'students.csv'", "not supported");
}

/* =================================================================
 * 4. table_reference
 * ================================================================= */
static void test_table_reference(void) {
    printf("--- table_reference\n");
    test_query("SELECT * FROM 'students.csv'", 5);                               /* string literal */
    test_query_any("SELECT * FROM students");                                    /* bare identifier (resolves to students.csv) */
    test_query_any("SELECT * FROM \"students.csv\"");                            /* double-quoted identifier */
    test_query_any("SELECT * FROM \"my data.csv\"");                             /* path with a space */
    test_query_error("SELECT * FROM (SELECT * FROM 'students.csv') AS t", "Expected"); /* subquery in FROM */
}

/* =================================================================
 * 5. alias
 * ================================================================= */
static void test_alias(void) {
    printf("--- alias\n");
    test_query_any("SELECT name AS n FROM 'students.csv'");
    test_query_any("SELECT name n FROM 'students.csv'");
    test_query_any("SELECT age * 2 AS double_age FROM 'students.csv'");
}

/* =================================================================
 * 6. expression – bitwise_expr
 * ================================================================= */
static void test_bitwise_expr(void) {
    printf("--- bitwise_expr (& | ^)\n");
    test_query_any("SELECT 6 & 3 FROM 'students.csv'");
    test_query_any("SELECT 6 | 3 FROM 'students.csv'");
    test_query_any("SELECT 6 ^ 3 FROM 'students.csv'");
    test_query_any("SELECT 6 & 3 | 1 FROM 'students.csv'");
    test_query_any("SELECT age & 3 FROM 'students.csv'");
    test_query_any("SELECT age | 1 FROM 'students.csv'");
    test_query_any("SELECT age ^ 2 FROM 'students.csv'");
}

/* =================================================================
 * 7. expression – additive_expr
 * ================================================================= */
static void test_additive_expr(void) {
    printf("--- additive_expr (+ -)\n");
    test_query_any("SELECT age + 1 FROM 'students.csv'");
    test_query_any("SELECT age - 1 FROM 'students.csv'");
    test_query_any("SELECT age + 1 + 1 FROM 'students.csv'");
    test_query_any("SELECT age + 1 - 2 FROM 'students.csv'");
}

/* =================================================================
 * 8. expression – multiplicative_expr
 * ================================================================= */
static void test_multiplicative_expr(void) {
    printf("--- multiplicative_expr (* / %%)\n");
    test_query_any("SELECT age * 2 FROM 'students.csv'");
    test_query_any("SELECT age / 2 FROM 'students.csv'");
    test_query_any("SELECT age % 3 FROM 'students.csv'");
    test_query_any("SELECT age * 2 / 4 FROM 'students.csv'");
}

/* =================================================================
 * 9. expression – operator precedence
 * ================================================================= */
static void test_precedence(void) {
    printf("--- expression precedence\n");
    test_query_any("SELECT 1 + 2 * 3 FROM 'students.csv'");        /* 7, not 9 */
    test_query_any("SELECT (1 + 2) * 3 FROM 'students.csv'");      /* 9 */
    test_query_any("SELECT 1 * 2 + 3 FROM 'students.csv'");        /* 5 */
    test_query_any("SELECT 1 + 2 * 3 - 4 / 2 FROM 'students.csv'"); /* 1+6-2 = 5 */
    test_query_any("SELECT 1 & 2 + 3 FROM 'students.csv'");        /* & after + : 1 & 5 = 1 */
}

/* =================================================================
 * 10. unary + / -
 * ================================================================= */
static void test_unary(void) {
    printf("--- unary + / -\n");
    test_query_any("SELECT -age FROM 'students.csv'");
    test_query_any("SELECT +age FROM 'students.csv'");
    test_query_any("SELECT --age FROM 'students.csv'");
    test_query_any("SELECT -age + 1 FROM 'students.csv'");
}

/* =================================================================
 * 11. function_call
 * ================================================================= */
static void test_function_calls(void) {
    printf("--- function_call\n");
    test_query_any("SELECT LENGTH('hello') FROM 'students.csv'");
    test_query_any("SELECT UPPER(name) FROM 'students.csv'");
    test_query_any("SELECT LOWER(city) FROM 'students.csv'");
    test_query_any("SELECT SUBSTR(name, 1, 3) FROM 'students.csv'");
    test_query_any("SELECT TRIM(city) FROM 'students.csv'");
    test_query_any("SELECT COALESCE(NULL, 'hi') FROM 'students.csv'");
    test_query_any("SELECT IFNULL(NULL, 'hi') FROM 'students.csv'");
    test_query_any("SELECT ABS(-5) FROM 'students.csv'");
    test_query_any("SELECT ROUND(3.14159, 2) FROM 'students.csv'");
    test_query_any("SELECT LENGTH(UPPER(name)) FROM 'students.csv'");
    test_query_any("SELECT CONCAT(name, ' ', city) FROM 'students.csv'");
    test_query_any("SELECT UCASE(name) FROM 'students.csv'");
    test_query_any("SELECT LCASE(city) FROM 'students.csv'");
    test_query_any("SELECT SUBSTRING(name, 2, 2) FROM 'students.csv'");
    /* aggregate functions */
    test_query("SELECT COUNT(*) FROM 'students.csv'", 1);
    test_query("SELECT SUM(age) FROM 'students.csv'", 1);
    test_query("SELECT AVG(age) FROM 'students.csv'", 1);
    test_query("SELECT MIN(age) FROM 'students.csv'", 1);
    test_query("SELECT MAX(age) FROM 'students.csv'", 1);
    test_query("SELECT COUNT(*) FROM 'students.csv' WHERE age > 20", 1);
    test_query("SELECT COUNT(name) FROM 'students.csv'", 1);
    test_query("SELECT MAX(name) FROM 'students.csv'", 1);
    test_query("SELECT ROUND(AVG(age), 1) FROM 'students.csv'", 1);
    test_query("SELECT MAX(age) + 1 FROM 'students.csv'", 1);
    test_query("SELECT SUM(age) FROM 'students.csv' WHERE age > 99", 1);      /* NULL -> 1 row */
    test_query("SELECT COUNT(*), MAX(age) FROM 'students.csv'", 1);
    test_query("SELECT 'Total:', COUNT(*) FROM 'students.csv'", 1);           /* literal + aggregate */
    test_query("SELECT COUNT(DISTINCT city) FROM 'students.csv'", 1);
    test_query("SELECT SUM(DISTINCT age) FROM 'students.csv'", 1);
    /* Value-aware DISTINCT on mixed data: '5' and '05' collapse to one value,
   so COUNT(DISTINCT val) is 7 and the numeric sum drops the duplicate. */
    test_query_value("SELECT COUNT(DISTINCT val) FROM 'distinct.csv'", "7");
    test_query_value("SELECT SUM(DISTINCT val) FROM 'distinct.csv'", "28.5");
    test_query_value("SELECT AVG(DISTINCT val) FROM 'distinct.csv'", "5.7");
    test_query("SELECT grp, COUNT(DISTINCT val) FROM 'distinct.csv' GROUP BY grp", 3);
    test_query("SELECT MAX(age) FROM 'students.csv' LIMIT 1", 1);
    test_query_error("SELECT name, MAX(age) FROM 'students.csv'", "GROUP BY");
    test_query_error("SELECT MAX(age) FROM 'students.csv' WHERE SUM(age) > 5", "not supported");
    test_query_error("SELECT COUNT(DISTINCT *) FROM 'students.csv'", "cannot be applied");
    test_query_error("SELECT UPPER(DISTINCT name) FROM 'students.csv'", "DISTINCT is only allowed");
    /* unknown function */
    test_query_error("SELECT nonexistent(name) FROM 'students.csv'", "Unknown function");
}

/* =================================================================
 * 12. group_by / having
 * ================================================================= */
static void test_group_by_having(void) {
    printf("--- group_by / having\n");
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT city, MAX(age) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT age, COUNT(*) FROM 'students.csv' GROUP BY age", 5);
    test_query("SELECT city FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT city, SUM(age) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT city, AVG(age) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT LOWER(city), COUNT(*) FROM 'students.csv' GROUP BY LOWER(city)", 3);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city HAVING COUNT(*) > 1", 2);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city HAVING COUNT(*) < 2", 1);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city HAVING city = 'LA'", 1);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city ORDER BY COUNT(*) DESC", 3);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city LIMIT 2", 2);
    test_query("SELECT city, COUNT(DISTINCT name) FROM 'students.csv' GROUP BY city", 3);
    test_query("SELECT COUNT(*) FROM 'students.csv' HAVING COUNT(*) > 1", 1);
    test_query("SELECT COUNT(*) FROM 'students.csv' WHERE age > 20 HAVING COUNT(*) > 1", 1);
    test_query("SELECT COUNT(*) FROM 'students.csv' WHERE age > 99 HAVING COUNT(*) > 1", 0);
    test_query("SELECT city, COUNT(*) FROM 'students.csv' WHERE age > 20 GROUP BY city", 2);
    /* validation errors */
    test_query_error("SELECT name FROM 'students.csv' GROUP BY city", "GROUP BY");
    test_query_error("SELECT * FROM 'students.csv' GROUP BY city", "GROUP BY");
    test_query_error("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city HAVING name = 'x'", "GROUP BY");
    test_query_error("SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city ORDER BY name", "GROUP BY");
    test_query_error("SELECT * FROM 'students.csv' HAVING age > 0", "HAVING without GROUP BY");
}

/* =================================================================
 * 13. case_expression
 * ================================================================= */
static void test_case_expression(void) {
    printf("--- case_expression\n");
    /* searched CASE */
    test_query_any("SELECT CASE WHEN 1 < 2 THEN 'yes' END FROM 'students.csv'");
    test_query_any(
        "SELECT CASE WHEN age < 21 THEN 'young' WHEN age < 23 THEN 'middle' ELSE 'old' END "
        "FROM 'students.csv'");
    /* simple CASE */
    test_query_any(
        "SELECT CASE city WHEN 'NYC' THEN 'NY' WHEN 'LA' THEN 'CA' ELSE city END "
        "FROM 'students.csv'");
    /* CASE without ELSE – yields NULL */
    test_query_any("SELECT CASE WHEN age < 20 THEN 'young' END FROM 'students.csv'");
    /* CASE inside expression */
    test_query_any("SELECT 1 + CASE WHEN 1 < 2 THEN 3 ELSE 4 END FROM 'students.csv'");
}

/* =================================================================
 * 14. qualified_identifier
 * ================================================================= */
static void test_qualified_identifier(void) {
    printf("--- qualified_identifier\n");
    test_query_any("SELECT students.name FROM 'students.csv'");
    test_query_any("SELECT * FROM 'students.csv' WHERE students.age > 20");
}

/* =================================================================
 * 15. double-quoted identifiers
 * ================================================================= */
static void test_quoted_identifier(void) {
    printf("--- quoted identifier\n");
    test_query_any("SELECT \"name\" FROM 'students.csv'");
    test_query_any("SELECT \"first name\" FROM \"my data.csv\"");
    test_query_any("SELECT \"select\" FROM \"my data.csv\"");        /* keyword as identifier */
    test_query_any("SELECT students.\"name\" FROM 'students.csv'"); /* qualified, quoted column */
    test_query_any("SELECT \"name\" AS \"display name\" FROM 'students.csv'");
    test_query_any("SELECT \"name\" FROM 'students.csv' WHERE \"name\" = 'Alice'");
    test_query_any("SELECT \"name\" FROM 'students.csv' ORDER BY \"name\"");
    test_query_any("SELECT \"name\" FROM 'students.csv' GROUP BY \"name\"");
    test_query_error("SELECT \"unterminated FROM 'students.csv'", "Unterminated identifier");
}

/* =================================================================
 * 16. literal
 * ================================================================= */
static void test_literals(void) {
    printf("--- literal\n");
    test_query_any("SELECT 42 FROM 'students.csv'");
    test_query_any("SELECT 3.14 FROM 'students.csv'");
    test_query_any("SELECT 'hello' FROM 'students.csv'");
    test_query_any("SELECT 'it''s a test' FROM 'students.csv'");
    test_query_error("SELECT 'unterminated", "Unterminated string");
    test_query_any("SELECT NULL FROM 'students.csv'");
    test_query_any("SELECT TRUE FROM 'students.csv'");
    test_query_any("SELECT FALSE FROM 'students.csv'");
}

/* =================================================================
 * 16. '(' expression ')'
 * ================================================================= */
static void test_parenthesized_expr(void) {
    printf("--- parenthesized expression\n");
    test_query_any("SELECT (age) FROM 'students.csv'");
    test_query_any("SELECT ((age)) FROM 'students.csv'");
    test_query_any("SELECT (age + 1) * 2 FROM 'students.csv'");
}

/* =================================================================
 * 17. '(' select_query ')' — scalar subquery
 * ================================================================= */
static void test_scalar_subquery(void) {
    printf("--- scalar subquery (unimplemented)\n");
    test_query_error("SELECT (SELECT age FROM 'students.csv') FROM 'students.csv'", "not supported");
}

/* =================================================================
 * 18. search_condition – NOT
 * ================================================================= */
static void test_search_not(void) {
    printf("--- NOT\n");
    test_query("SELECT * FROM 'students.csv' WHERE NOT age = 20", 4);
    test_query("SELECT * FROM 'students.csv' WHERE NOT (age = 20)", 4);
    test_query("SELECT * FROM 'students.csv' WHERE NOT age < 21 AND city = 'LA'", 2);
}

/* =================================================================
 * 19. comparison_operator
 * ================================================================= */
static void test_comparison_operators(void) {
    printf("--- comparison_operator\n");
    test_query("SELECT * FROM 'students.csv' WHERE age = 20", 1);
    test_query("SELECT * FROM 'students.csv' WHERE age != 20", 4);
    test_query("SELECT * FROM 'students.csv' WHERE age <> 20", 4);
    test_query("SELECT * FROM 'students.csv' WHERE age < 21", 2);
    test_query("SELECT * FROM 'students.csv' WHERE age > 21", 2);
    test_query("SELECT * FROM 'students.csv' WHERE age <= 21", 3);
    test_query("SELECT * FROM 'students.csv' WHERE age >= 21", 3);
    /* expressions on both sides */
    test_query("SELECT * FROM 'students.csv' WHERE age + 1 = 21", 1);
    test_query("SELECT * FROM 'students.csv' WHERE age = 10 + 10", 1);
    test_query("SELECT * FROM 'students.csv' WHERE age + 1 = age + 1", 5);
}

/* =================================================================
 * 20. IN / NOT IN
 * ================================================================= */
static void test_search_in(void) {
    printf("--- IN / NOT IN\n");
    test_query("SELECT * FROM 'students.csv' WHERE city IN ('NYC', 'LA')", 4);
    test_query("SELECT * FROM 'students.csv' WHERE city NOT IN ('NYC', 'LA')", 1);
    test_query("SELECT * FROM 'students.csv' WHERE age IN (20, 22)", 2);
    /* IN (subquery) – unimplemented; should error */
    test_query_error("SELECT * FROM 'students.csv' WHERE age IN (SELECT 20 FROM 'students.csv')", "not supported");
}

/* =================================================================
 * 21. BETWEEN
 * ================================================================= */
static void test_search_between(void) {
    printf("--- BETWEEN\n");
    test_query("SELECT * FROM 'students.csv' WHERE age BETWEEN 20 AND 22", 3);
    test_query("SELECT * FROM 'students.csv' WHERE age BETWEEN 20 AND 22 AND city = 'NYC'", 2);
}

/* =================================================================
 * 22. LIKE / ILIKE
 * ================================================================= */
static void test_search_like(void) {
    printf("--- LIKE / ILIKE\n");
    test_query("SELECT * FROM 'students.csv' WHERE name LIKE 'A%'", 1);
    test_query("SELECT * FROM 'students.csv' WHERE name LIKE '%e'", 3);
    test_query("SELECT * FROM 'students.csv' WHERE name LIKE '%li%'", 2);
    test_query("SELECT * FROM 'students.csv' WHERE name LIKE 'A____'", 1);
    test_query("SELECT * FROM 'students.csv' WHERE city ILIKE 'nyc'", 2);
    test_query("SELECT * FROM 'students.csv' WHERE city ILIKE 'l%'", 2);
}

/* =================================================================
 * 23. AND / OR / precedence
 * ================================================================= */
static void test_search_and_or(void) {
    printf("--- AND / OR\n");
    test_query("SELECT * FROM 'students.csv' WHERE age > 20 AND city = 'NYC'", 1);
    test_query("SELECT * FROM 'students.csv' WHERE age = 20 OR age = 22", 2);
    /* AND binds tighter than OR — 3 rows: (age>20 AND city=NYC)=Diana, OR city=LA=Bob+Eve => total 3 */
    test_query("SELECT * FROM 'students.csv' WHERE age > 20 AND city = 'NYC' OR city = 'LA'", 3);
    /* explicit parens – same result */
    test_query("SELECT * FROM 'students.csv' WHERE (age > 20 AND city = 'NYC') OR city = 'LA'", 3);
    /* different grouping – age>20 AND (city=NYC OR city=LA) => Bob(22,LA), Diana(21,NYC), Eve(23,LA) = 3 */
    test_query("SELECT * FROM 'students.csv' WHERE age > 20 AND (city = 'NYC' OR city = 'LA')", 3);
    /* chained AND */
    test_query("SELECT * FROM 'students.csv' WHERE age > 19 AND age < 22 AND city = 'NYC'", 2);
}

/* =================================================================
 * 24. '(' search_condition ')'
 * ================================================================= */
static void test_paren_search_condition(void) {
    printf("--- parenthesized search condition\n");
    test_query("SELECT * FROM 'students.csv' WHERE (age = 20)", 1);
    test_query("SELECT * FROM 'students.csv' WHERE ((age = 20))", 1);
}

/* =================================================================
 * 25. bare expression (truthy evaluation)
 * ================================================================= */
static void test_bare_expression_condition(void) {
    printf("--- bare expression condition (truthy)\n");
    test_query("SELECT * FROM 'students.csv' WHERE age", 5);
    test_query("SELECT * FROM 'students.csv' WHERE age - 20", 4);
    test_query("SELECT * FROM 'students.csv' WHERE name", 5);
}

/* =================================================================
 * 26. set_op (UNIMPLEMENTED)
 * ================================================================= */
static void test_set_op(void) {
    printf("--- set_op (unimplemented)\n");
    test_query_error("SELECT name FROM 'students.csv' UNION SELECT name FROM 'students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'students.csv' UNION ALL SELECT name FROM 'students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'students.csv' INTERSECT SELECT name FROM 'students.csv'",
                     "Unexpected token");
    test_query_error("SELECT name FROM 'students.csv' EXCEPT SELECT name FROM 'students.csv'",
                     "Unexpected token");
}

/* =================================================================
 * 27. Other statements (UNIMPLEMENTED)
 * ================================================================= */
static void test_other_statements(void) {
    printf("--- other statements (unimplemented)\n");
    test_query_error("INSERT INTO 'students.csv' VALUES ('x', 1, 'y')", "Expected 'SELECT'");
    test_query_error("UPDATE 'students.csv' SET age = 21", "Expected 'SELECT'");
    test_query_error("DELETE FROM 'students.csv'", "Expected 'SELECT'");
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
    test_query_parse_error_count("SELECT name age city FROM 'students.csv'", 2);
    test_query_parse_error_count("SELECT 1 2 3 FROM 'students.csv'", 2);

    /* Valid query has zero parse errors */
    test_query_parse_error_count("SELECT * FROM 'students.csv'", 0);

    /* Error limit is capped at 50 */
    test_query_parse_error_count("SELECT ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,, FROM 'students.csv'", 50);

    /* Runtime (semantic) errors still fail-fast and are not parse errors */
    test_query_error("SELECT FakeCol FROM 'students.csv'", "Column");
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
    test_parenthesized_expr();
    test_scalar_subquery();
    test_search_not();
    test_comparison_operators();
    test_search_in();
    test_search_between();
    test_search_like();
    test_search_and_or();
    test_paren_search_condition();
    test_bare_expression_condition();
    test_set_op();
    test_other_statements();
    test_error_recovery();

    printf("\n=== Results ===\n");
    printf("PASS: %d   FAIL: %d   TOTAL: %d\n", pass, fail, pass + fail);

    return fail > 0 ? 1 : 0;
}

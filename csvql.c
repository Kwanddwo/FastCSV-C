#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include "arena.h"
#include "csv_config.h"
#include "csv_reader.h"
#include "query/query.h"
#include "query/str_util.h"
#include "deps/linenoise.h"

#define QUERY_ARENA_SIZE (4 * 1024 * 1024)
#define CFG_ARENA_SIZE (2 * 1024)
#define CSVQL_VERSION "1.0"

/* ===== Arena size resolution =====
 * The query and config arenas can be sized at runtime via environment
 * variables (CSVQL_QUERY_ARENA_SIZE, CSVQL_CONFIG_ARENA_SIZE). Accepted
 * formats: a plain byte count ("1048576") or a number with a K/M/G suffix
 * ("256K", "64M", "1G", case-insensitive). On missing/invalid input the
 * built-in default is used.
 */
static size_t arena_size_from_env(const char *name, size_t default_size) {
    const char *raw = getenv(name);
    if (!raw || !*raw) return default_size;

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(raw, &end, 10);
    if (errno != 0 || end == raw) return default_size;

    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) {
        unsigned long long mult = 0;
        switch (toupper((unsigned char)*end)) {
            case 'K': mult = 1024ULL; break;
            case 'M': mult = 1024ULL * 1024ULL; break;
            case 'G': mult = 1024ULL * 1024ULL * 1024ULL; break;
            default: return default_size; /* trailing junk after suffix */
        }
        end++;
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) return default_size; /* extra characters after suffix */
        val *= mult;
    }

    if (val == 0 || val > (size_t)-1 / 2) return default_size;
    return (size_t)val;
}

/* ===== Terminal colors ===== */
#define C_RESET  "\x1b[0m"
#define C_RED    "\x1b[31m"
#define C_GREEN  "\x1b[32m"
#define C_CYAN   "\x1b[36m"
#define C_DIM    "\x1b[2m"

static bool use_color = false;

/* ===== Error rendering ===== */

/* Print the whole statement with a caret pointing at `col` (0-based byte
 * column) on line `line`, rustc-style:
 *
 *    1 | SELECT city,
 *    2 | name
 *    3 | FROM bad
 *      |       ^
 *
 * Best-effort: tabs are expanded to 8-column stops so the caret stays
 * aligned, and a `col` past the end of the line clamps to end of line.
 * Prints nothing when the source or the target line cannot be located. */
static void print_error_location(const char *source, int line, int col) {
    if (source == NULL || line < 1) return;
    if (col < 0) col = 0;

    int total_lines = 1;
    for (const char *q = source; *q; q++) {
        if (*q == '\n') total_lines++;
    }
    if (line > total_lines) return; /* line out of range */

    char numbuf[16];
    snprintf(numbuf, sizeof(numbuf), "%d", total_lines);
    int width = (int)strlen(numbuf);

    const char *dim = use_color ? C_DIM : "";
    const char *red = use_color ? C_RED : "";
    const char *rst = use_color ? C_RESET : "";

    fprintf(stderr, "%s%*s |%s\n", dim, width, "", rst);

    const char *p = source;
    for (int ln = 1; ln <= total_lines; ln++) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        size_t len = (size_t)(end - p);
        while (len > 0 && p[len - 1] == '\r') len--;

        char num[16];
        snprintf(num, sizeof(num), "%d", ln);
        fprintf(stderr, "%s%*s%s | %s%.*s%s\n",
                dim, width, num, rst, "", (int)len, p, rst);

        if (ln == line) {
            int vis = 0;
            size_t i = 0;
            while (i < len && (int)i < col) {
                if (p[i] == '\t') vis = (vis / 8 + 1) * 8;
                else vis++;
                i++;
            }
            fprintf(stderr, "%s%*s | %s%*s%s^%s\n", dim, width, "", rst, vis, "", red, rst);
        }

        if (!*end) break;
        p = end + 1;
    }
}

/* ===== Result printing ===== */
static void print_separator(int *widths, int count) {
    printf("+");
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < widths[i] + 2; j++) printf("-");
        printf("+");
    }
    printf("\n");
}

static void print_result(QueryResult *result, const char *source) {
    const char *red = use_color ? C_RED : "";
    const char *rst = use_color ? C_RESET : "";

    if (result->parse_errors && result->parse_errors->count > 0) {
        for (int i = 0; i < result->parse_errors->count; i++) {
            int line = result->parse_errors->error_lines[i];
            int col  = result->parse_errors->error_columns[i];
            fprintf(stderr, "%sError at [line %d, col %d]:%s %s\n",
                    red, line, col, rst, result->parse_errors->errors[i]);
            print_error_location(source, line, col);
            if (i + 1 < result->parse_errors->count) fprintf(stderr, "\n");
        }
        return;
    }

    if (result->error) {
        if (result->error_line > 0 || result->error_column >= 0) {
            fprintf(stderr, "%sError at [line %d, col %d]:%s %s\n",
                    red, result->error_line, result->error_column, rst, result->error);
            print_error_location(source, result->error_line, result->error_column);
        } else {
            fprintf(stderr, "%sError:%s %s\n", red, rst, result->error);
        }
        return;
    }

    if (result->header_count == 0) {
        printf("Empty set.\n");
        return;
    }

    int widths[result->header_count];
    for (int i = 0; i < result->header_count; i++) {
        widths[i] = (int)strlen(result->headers[i]);
    }
    for (int i = 0; i < result->record_count; i++) {
        CSVRecord *rec = result->records[i];
        for (int j = 0; j < result->header_count && j < (int)rec->field_count; j++) {
            int len = (int)strlen(rec->fields[j]);
            if (len > widths[j]) widths[j] = len;
        }
    }

    print_separator(widths, result->header_count);
    printf("|");
    for (int i = 0; i < result->header_count; i++) {
        printf(" %-*s |", widths[i], result->headers[i]);
    }
    printf("\n");
    print_separator(widths, result->header_count);

    for (int i = 0; i < result->record_count; i++) {
        CSVRecord *rec = result->records[i];
        printf("|");
        for (int j = 0; j < result->header_count; j++) {
            if (j < (int)rec->field_count) {
                printf(" %-*s |", widths[j], rec->fields[j]);
            } else {
                printf(" %-*s |", widths[j], "");
            }
        }
        printf("\n");
    }
    print_separator(widths, result->header_count);

    if (use_color) {
        printf("%s%d row(s) in set.%s\n", C_DIM, result->record_count, C_RESET);
    } else {
        printf("%d row(s) in set.\n", result->record_count);
    }
}

/* ===== Growable string buffer for multi-line statements ===== */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_append(StrBuf *sb, const char *s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        size_t newcap = sb->cap ? sb->cap : 128;
        while (newcap < sb->len + n + 1) newcap *= 2;
        sb->data = realloc(sb->data, newcap);
        sb->cap = newcap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_reset(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

/* Trim leading/trailing whitespace in place; returns pointer into the string. */
static char* trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/* True when the buffered statement is finished: the last significant
 * character outside of a string literal is ';'. An unclosed single-quote
 * keeps reading (strings may span lines). */
static bool statement_complete(const char *buf) {
    bool in_string = false;
    int last_non_space = -1;
    for (int i = 0; buf[i]; i++) {
        char c = buf[i];
        if (c == '\'') in_string = !in_string;
        if (!in_string && !isspace((unsigned char)c)) last_non_space = i;
    }
    if (in_string) return false;
    if (last_non_space < 0) return true; /* whitespace only */
    return buf[last_non_space] == ';';
}

/* Run one statement, resizing the query arena when the estimated result set
   exceeds its current capacity. The arena is reset when it already fits. */
static QueryResult execute_sql(CSVConfig *config, Arena *query_arena, const char *sql) {
    size_t need = query_estimate_result_size(sql);
    if (need > query_arena->total_size) {
        Arena fresh;
        if (arena_create(&fresh, need) == ARENA_OK) {
            arena_destroy(query_arena);
            *query_arena = fresh;
        }
    } else {
        arena_reset(query_arena);
    }
    return query_execute(config, sql, query_arena);
}

/* Execute one or more ';'-separated statements. */
static void exec_statement(CSVConfig *config, Arena *arena, const char *stmt) {
    const char *p = stmt;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (!*p) break;

        const char *start = p;
        bool in_string = false;
        while (*p) {
            if (*p == '\'') in_string = !in_string;
            if (*p == ';' && !in_string) break;
            p++;
        }

        size_t seglen = (size_t)(p - start);
        while (seglen > 0 && isspace((unsigned char)start[seglen - 1])) seglen--;
        if (seglen > 0) {
            char *seg = malloc(seglen + 1);
            memcpy(seg, start, seglen);
            seg[seglen] = '\0';

            QueryResult res = execute_sql(config, arena, seg);
            print_result(&res, seg);
            printf("\n");

            free(seg);
        }
        if (*p == ';') p++;
    }
}

/* ===== Meta commands ===== */
static void print_help(void) {
    printf("FastCSV-C query shell\n");
    printf("----------------------\n");
    printf("Commands:\n");
    printf("  \\h, \\help, .help       Show this help\n");
    printf("  \\q, \\quit, \\exit      Quit the shell\n");
    printf("  exit, quit             Quit the shell\n");
    printf("  \\clear, clear          Clear the screen\n");
    printf("  \\echo <text>           Print text\n");
    printf("  \\version               Show version information\n");
    printf("  Ctrl-D                 Quit\n");
    printf("  Ctrl-C                 Cancel the current statement\n");
    printf("\n");
    printf("SQL statements end with ';'. A blank line also executes the\n");
    printf("pending statement. Example:\n");
    printf("  SELECT city, COUNT(*) FROM 'students.csv' GROUP BY city;\n");
    printf("\n");
    printf("Supported: SELECT with DISTINCT, * , expressions, functions,\n");
    printf("aggregates (COUNT/SUM/AVG/MIN/MAX with DISTINCT), WHERE,\n");
    printf("GROUP BY, HAVING, ORDER BY, LIMIT/OFFSET, CASE, IN, BETWEEN,\n");
    printf("LIKE/ILIKE. Table names are quoted paths, e.g. FROM 'data.csv'.\n");
    printf("\n");
    printf("History is saved to ~/.csvql_history (override with $CSVQL_HISTORY).\n");
}

static bool is_quit_cmd(const char *t) {
    return str_ieq(t, "\\q") ||
           str_ieq(t, "\\quit") ||
           str_ieq(t, "\\exit") ||
           str_ieq(t, "/exit") ||
           str_ieq(t, "exit") ||
           str_ieq(t, "quit") ||
           str_ieq(t, ".quit") ||
           str_ieq(t, ".exit");
}

/* Returns true when the line was recognized as a meta command. */
static bool handle_meta(const char *t) {
    if (str_ieq(t, "\\h") || str_ieq(t, "\\help") ||
        str_ieq(t, ".help")) {
        print_help();
        return true;
    }
    if (str_ieq(t, "\\clear") || str_ieq(t, "clear")) {
        linenoiseClearScreen();
        return true;
    }
    if (str_nieq(t, "\\echo", 5) && (t[5] == '\0' || isspace((unsigned char)t[5]))) {
        const char *p = t + 5;
        while (*p && isspace((unsigned char)*p)) p++;
        printf("%s\n", p);
        return true;
    }
    if (str_ieq(t, "\\version")) {
        printf("csvql %s (FastCSV-C)\n", CSVQL_VERSION);
        return true;
    }
    return false;
}

/* ===== History ===== */
static const char* history_path(void) {
    static char path[1024];
    const char *env = getenv("CSVQL_HISTORY");
    if (env && *env) {
        if (env[0] == '~' && env[1] == '/') {
            const char *home = getenv("HOME");
            if (!home) return env;
            snprintf(path, sizeof(path), "%s%s", home, env + 1);
            return path;
        }
        return env;
    }
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof(path), "%s/.csvql_history", home);
    return path;
}

static void maybe_add_history(const char *stmt) {
    const char *s = stmt;
    while (*s && (isspace((unsigned char)*s) || *s == ';')) s++;
    if (*s == '\0') return; /* nothing but ';' / whitespace */
    char *copy = strdup(stmt);
    if (!copy) return;
    char *e = copy + strlen(copy);
    while (e > copy && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    /* Fold newlines outside string literals into spaces so multi-line
     * statements are recalled as a single line. */
    int in_string = 0;
    for (char *p = copy; *p; p++) {
        if (*p == '\'') in_string = !in_string;
        else if (*p == '\n' && !in_string) *p = ' ';
    }
    linenoiseHistoryAdd(copy);
    free(copy);
}

/* ===== Tab completion ===== */
static void completion_callback(const char *buf, linenoiseCompletions *lc) {
    static const char *words[] = {
        "SELECT", "FROM", "WHERE", "GROUP", "BY", "HAVING", "ORDER", "LIMIT",
        "OFFSET", "DISTINCT", "AND", "OR", "NOT", "IN", "BETWEEN", "LIKE",
        "ILIKE", "AS", "NULL", "TRUE", "FALSE", "CASE", "WHEN", "THEN", "ELSE",
        "END", "ASC", "DESC", "UNION", "EXCEPT", "INTERSECT",
        "COUNT", "SUM", "AVG", "MIN", "MAX",
        "UPPER", "UCASE", "LOWER", "LCASE", "LENGTH", "TRIM", "SUBSTR",
        "SUBSTRING", "CONCAT", "COALESCE", "IFNULL", "ABS", "ROUND",
        NULL
    };
    size_t len = strlen(buf);
    while (len > 0 && (isalnum((unsigned char)buf[len - 1]) || buf[len - 1] == '_'))
        len--;
    const char *tok = buf + len;
    size_t tlen = strlen(tok);
    for (int i = 0; words[i]; i++) {
        if (tlen == 0 || str_nieq(words[i], tok, tlen))
            linenoiseAddCompletion(lc, words[i]);
    }
}

/* ===== REPL ===== */
static void repl_loop(CSVConfig *config, Arena *query_arena) {
    linenoiseHistorySetMaxLen(1000);
    linenoiseSetCompletionCallback(completion_callback);

    const char *hist = history_path();
    if (hist) linenoiseHistoryLoad(hist);

    const char *primary = use_color ? C_GREEN "csvql> " C_RESET : "csvql> ";
    const char *cont    = use_color ? C_CYAN "...> "  C_RESET : "...> ";

    printf("FastCSV-C query shell\n");
    printf("Type \\h for help, exit or \\q to quit.\n\n");

    StrBuf sb = {0};
    bool done = false;

    while (!done) {
        const char *prompt = sb.len ? cont : primary;

        errno = 0;
        char *line = linenoise(prompt);
        if (line == NULL) {
            if (errno == EAGAIN) {
                /* Ctrl-C: cancel the current line / pending statement */
                if (sb.len) {
                    printf("^C\n");
                    sb_reset(&sb);
                }
                continue;
            }
            /* EOF (Ctrl-D) or end of piped input */
            printf("\n");
            if (sb.len) exec_statement(config, query_arena, sb.data);
            break;
        }

        char *t = trim_ws(line);

        if (sb.len == 0) {
            if (*t == '\0') {
                linenoiseFree(line);
                continue;
            }
            if (is_quit_cmd(t)) {
                done = true;
                linenoiseFree(line);
                break;
            }
            if (str_ieq(t, "clear")) {
                linenoiseClearScreen();
                linenoiseFree(line);
                continue;
            }
            if (t[0] == '\\' || str_nieq(t, ".help", 5) ||
                str_ieq(t, ".quit") || str_ieq(t, ".exit")) {
                if (!handle_meta(t))
                    printf("Unknown command: %s (try \\h)\n", t);
                linenoiseFree(line);
                continue;
            }
        } else if (is_quit_cmd(t)) {
            done = true;
            linenoiseFree(line);
            break;
        }

        sb_append(&sb, line, strlen(line));
        sb_append(&sb, "\n", 1);

        /* Execute when complete, or on a blank line (semicolon optional). */
        bool blank_line = (*t == '\0');
        if (statement_complete(sb.data) || (blank_line && sb.len > 1)) {
            maybe_add_history(sb.data);
            exec_statement(config, query_arena, sb.data);
            sb_reset(&sb);
        }

        linenoiseFree(line);
    }

    free(sb.data);
    if (hist) linenoiseHistorySave(hist);
}

/* ===== Entry point ===== */
int main(int argc, char **argv) {
    Arena cfg_arena;
    Arena query_arena;

    size_t cfg_size = arena_size_from_env("CSVQL_CONFIG_ARENA_SIZE", CFG_ARENA_SIZE);
    size_t query_size = arena_size_from_env("CSVQL_QUERY_ARENA_SIZE", QUERY_ARENA_SIZE);

    if (arena_create(&cfg_arena, cfg_size) != ARENA_OK) {
        fprintf(stderr, "Failed to create config arena "
                        "(sized %zu bytes; see CSVQL_CONFIG_ARENA_SIZE).\n", cfg_size);
        return 1;
    }

    if (arena_create(&query_arena, query_size) != ARENA_OK) {
        fprintf(stderr, "Failed to create query arena "
                        "(sized %zu bytes; see CSVQL_QUERY_ARENA_SIZE).\n", query_size);
        arena_destroy(&cfg_arena);
        return 1;
    }

    CSVConfig *config = csv_config_create(&cfg_arena);
    csv_config_set_has_header(config, true);

    use_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;

    if (argc == 2) {
        QueryResult result = execute_sql(config, &query_arena, argv[1]);
        print_result(&result, argv[1]);
        if (result.error) {
            arena_destroy(&cfg_arena);
            arena_destroy(&query_arena);
            return 1;
        }
    } else if (argc == 1) {
        repl_loop(config, &query_arena);
    } else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s            Enter interactive REPL\n", argv[0]);
        fprintf(stderr, "  %s \"<sql>\"   Run a single query and exit\n", argv[0]);
        arena_destroy(&cfg_arena);
        arena_destroy(&query_arena);
        return 1;
    }

    arena_destroy(&cfg_arena);
    arena_destroy(&query_arena);
    return 0;
}

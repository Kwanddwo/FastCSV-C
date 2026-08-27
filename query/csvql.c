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
#include "scanner.h"
#include "deps/linenoise.h"
#include <locale.h>

#define CFG_ARENA_SIZE (2 * 1024)
#define CSVQL_VERSION "1.0"

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

    /* Heap-allocated: the header count is user data (a wide CSV or a big
       star expansion) and must not become a stack allocation. */
    int *widths = malloc(sizeof(int) * (size_t)result->header_count);
    if (widths == NULL) {
        fprintf(stderr, "%sError:%s Out of memory.\n", red, rst);
        return;
    }
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
    free(widths);

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

/* ===== Comment-aware text scanning =====
 * The shell reads raw SQL text: deciding when a statement is complete,
 * splitting ';'-separated statements, and folding history lines. All three
 * must use the real lexer (scanner.h) rather than a shadow tokenizer, or
 * strings, quoted identifiers and comments would drift apart. */

/* One pass through the real scanner over a NUL-terminated buffer. Sets
   *saw_code (any token before EOF; comments are invisible to the lexer),
   *incomplete (the lexer stopped inside a string/identifier/comment open at
   end of input) and *last_type (last token type before EOF). */
static void text_scan(const char *text, bool *saw_code, bool *incomplete,
                      TokenType *last_type) {
    Scanner sc = scanner_init(text);
    bool saw = false;
    TokenType last = TOKEN_EOF;
    for (;;) {
        Token t = scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        saw = true;
        last = t.type;
    }
    if (saw_code) *saw_code = saw;
    if (incomplete) *incomplete = sc.incomplete;
    if (last_type) *last_type = last;
}

/* True when the trimmed line is only a comment: it behaves like a blank
   line in the REPL (it executes the pending statement). An unterminated
   block comment still counts as a comment here: the statement-completeness
   check keeps reading until it closes. */
static bool comment_only_line(const char *t) {
    bool saw;
    text_scan(t, &saw, NULL, NULL);
    return !saw;
}

/* True when the buffered statement is finished: no construct left open and
   the last significant token (outside strings, quoted identifiers and
   comments) is ';'. Unterminated strings and block comments keep reading
   (both may span lines). */
static bool statement_complete(const char *buf) {
    bool saw, incomplete;
    TokenType last;
    text_scan(buf, &saw, &incomplete, &last);
    if (incomplete) return false;
    if (!saw) return true; /* whitespace/comments only */
    return last == TOKEN_SEMICOLON;
}

/* Execute one ';'-delimited segment [start,end). Whitespace-only and
   comment-only segments are skipped. */
static void run_segment(CSVConfig *config, const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end <= start) return;

    size_t seglen = (size_t)(end - start);
    char *seg = malloc(seglen + 1);
    if (seg == NULL) return;
    memcpy(seg, start, seglen);
    seg[seglen] = '\0';

    bool saw;
    text_scan(seg, &saw, NULL, NULL);
    if (!saw) { free(seg); return; } /* comments only */

    QueryResult res = query_execute(config, seg);
    print_result(&res, seg);
    printf("\n");
    query_result_destroy(&res);

    free(seg);
}

/* Execute one or more ';'-separated statements. The real lexer delivers a
   TOKEN_SEMICOLON only at the top level: a ';' inside a string literal, a
   double-quoted identifier ("my;data.csv") or a comment never splits. */
static void exec_statement(CSVConfig *config, const char *stmt) {
    Scanner sc = scanner_init(stmt);
    const char **semi_ends = NULL;
    int semi_count = 0;
    int semi_cap = 0;

    for (;;) {
        Token t = scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        if (t.type == TOKEN_SEMICOLON) {
            if (semi_count >= semi_cap) {
                int new_cap = semi_cap ? semi_cap * 2 : 8;
                const char **ne = (const char**)realloc(
                    semi_ends, sizeof(char*) * (size_t)new_cap);
                if (ne == NULL) break;
                semi_ends = ne;
                semi_cap = new_cap;
            }
            semi_ends[semi_count++] = t.lexeme + t.length;
        }
    }

    const char *seg_start = stmt;
    for (int i = 0; i < semi_count; i++) {
        /* The segment is the text before the ';' terminator, which is never
           part of the statement. */
        run_segment(config, seg_start, semi_ends[i] - 1);
        seg_start = semi_ends[i];
    }
    run_segment(config, seg_start, stmt + strlen(stmt));

    free(semi_ends);
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

/* True when the byte range [start,end) contains a comment marker. Gaps
   between lexer tokens are composed exclusively of whitespace and comments,
   so two-char "dash dash" and "slash star" can only appear as comment
   openers. */
static bool gap_has_comment(const char *start, const char *end) {
    for (const char *p = start; p + 1 < end; p++) {
        if (p[0] == '-' && p[1] == '-') return true;
        if (p[0] == '/' && p[1] == '*') return true;
    }
    return false;
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

    /* Fold newlines outside strings, quoted identifiers and comments into
       spaces so multi-line statements are recalled as a single line. The
       gaps between real tokens contain only whitespace/comments (the lexer
       consumed them), so a raw comment-marker check on a gap is exact; the
       tokens themselves are copied verbatim, including multi-line strings
       and double-quoted identifiers. */
    Scanner sc = scanner_init(stmt);
    char *out = copy;
    const char *prev_end = stmt;
    for (;;) {
        Token t = scan_token(&sc);
        if (t.type == TOKEN_EOF) {
            /* Trailing gap (whitespace / comments / unterminated comment) */
            bool has_comment = gap_has_comment(prev_end, copy + strlen(copy));
            for (const char *p = prev_end; *p; p++) {
                if (*p == '\n' && !has_comment) *out++ = ' ';
                else *out++ = *p;
            }
            break;
        }

        /* Quoted identifiers reset the scanner start to the content, so the
           consumed span includes the quotes on both sides. */
        bool quoted = (t.lexeme > stmt && t.lexeme[-1] == '"');
        const char *span_begin = quoted ? t.lexeme - 1 : t.lexeme;
        const char *span_end = span_begin + (quoted ? t.length + 2 : t.length);

        bool has_comment = gap_has_comment(prev_end, span_begin);
        for (const char *p = prev_end; p < span_begin; p++) {
            if (*p == '\n' && !has_comment) *out++ = ' ';
            else *out++ = *p;
        }
        for (const char *p = span_begin; p < span_end; p++) *out++ = *p;
        prev_end = span_end;
    }
    *out = '\0';
    linenoiseHistoryAdd(copy);
    free(copy);
}

/* ===== REPL ===== */
static void repl_loop(CSVConfig *config) {
    linenoiseHistorySetMaxLen(1000);

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
            if (sb.len) exec_statement(config, sb.data);
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

        /* Execute when complete, or on a blank/comment-only line
           (semicolon optional). */
        bool blank_line = (*t == '\0') || comment_only_line(t);
        if (statement_complete(sb.data) || (blank_line && sb.len > 1)) {
            maybe_add_history(sb.data);
            exec_statement(config, sb.data);
            sb_reset(&sb);
        }

        linenoiseFree(line);
    }

    free(sb.data);
    if (hist) linenoiseHistorySave(hist);
}

/* True when the text contains anything other than whitespace and comments.
   Used to skip comment-only input in one-shot mode. */
static bool has_significant_code(const char *s) {
    bool saw;
    text_scan(s, &saw, NULL, NULL);
    return saw;
}

/* ===== Entry point ===== */
int main(int argc, char **argv) {
    Arena cfg_arena;

    /* Numbers parse and print with '.' regardless of the user's environment:
       under a comma-decimal locale, strtod and %.15g would silently read
       '1.5' cells as text and print 1,5. */
    setlocale(LC_NUMERIC, "C");

    if (arena_create(&cfg_arena, CFG_ARENA_SIZE) != ARENA_OK) {
        fprintf(stderr, "Failed to create config arena.\n");
        return 1;
    }

    CSVConfig *config = csv_config_create(&cfg_arena);
    csv_config_set_has_header(config, true);

    use_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;

    if (argc == 2) {
        if (!has_significant_code(argv[1])) {
            arena_destroy(&cfg_arena);
            return 0;
        }
        QueryResult result = query_execute(config, argv[1]);
        print_result(&result, argv[1]);
        bool failed = result.error != NULL;
        query_result_destroy(&result);
        arena_destroy(&cfg_arena);
        return failed ? 1 : 0;
    } else if (argc == 1) {
        repl_loop(config);
    } else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s            Enter interactive REPL\n", argv[0]);
        fprintf(stderr, "  %s \"<sql>\"   Run a single query and exit\n", argv[0]);
        arena_destroy(&cfg_arena);
        return 1;
    }

    arena_destroy(&cfg_arena);
    return 0;
}

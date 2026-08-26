#ifndef QUERY_DATE_H
#define QUERY_DATE_H

#include "eval.h"
#include <stdbool.h>

/* ===== ISO-8601 string dates =====
   Dates are plain strings in this engine ('YYYY-MM-DD', optionally followed
   by ' HH:MM:SS'), which already compare chronologically under the existing
   lexicographic comparison. All parsing below range-validates the fields. */

/* Parse 'YYYY-MM-DD[ HH:MM:SS]'. Returns false (and leaves outputs
   untouched) for any malformed or out-of-range input. */
bool parse_iso_datetime(const char *s, bool *has_time,
                        int *year, int *month, int *day,
                        int *hour, int *minute, int *second);

bool is_valid_iso_date(const char *s);
bool is_valid_iso_time(const char *s);
bool is_valid_iso_timestamp(const char *s);

/* True when f names a supported EXTRACT field. */
bool is_valid_extract_field(const char *f);

/* ===== Value functions ===== */
enum {
    DT_CURRENT_DATE = 1,
    DT_CURRENT_TIME,
    DT_CURRENT_TIMESTAMP,
    DT_LOCALTIME,
    DT_LOCALTIMESTAMP,
};

/* CURRENT_DATE / CURRENT_TIME / CURRENT_TIMESTAMP / LOCALTIME /
   LOCALTIMESTAMP as of now (local time). */
EvalResult eval_datetime_value(int kind, QArena *arena);

/* EXTRACT(field FROM value) and the YEAR()/MONTH()/DAY() convenience forms.
   A non-parseable value yields NULL. */
EvalResult eval_date_part(const char *field, const EvalResult *v);

/* DATEDIFF(a, b) = whole days from b to a (a - b), leap-year aware. */
EvalResult eval_datediff(const EvalResult *a, const EvalResult *b);

#endif
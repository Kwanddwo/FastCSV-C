/* ISO-8601 string-date helpers and the standard datetime value functions
 * (CURRENT_DATE, ...), EXTRACT and the documented date extensions
 * (NOW, YEAR(), MONTH(), DAY(), DATEDIFF, EXTRACT(QUARTER)). Dates are
 * plain 'YYYY-MM-DD[ HH:MM:SS]' strings with no separate date type. */
#include "date.h"
#include "str_util.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m) {
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap_year(y)) return 29;
    return days[m - 1];
}

bool parse_iso_datetime(const char *s, bool *has_time,
                        int *year, int *month, int *day,
                        int *hour, int *minute, int *second) {
    if (s == NULL) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    int consumed = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &y, &mo, &d, &consumed) != 3)
        return false;
    const char *p = s + consumed;
    bool ht = false;
    if (*p == ' ') {
        int c2 = 0;
        if (sscanf(p, " %2d:%2d:%2d%n", &h, &mi, &se, &c2) != 3)
            return false;
        ht = true;
        p += c2;
    }
    if (*p != '\0') return false;          /* trailing junk */
    if (mo < 1 || mo > 12) return false;
    if (d < 1 || d > days_in_month(y, mo)) return false;
    if (ht && (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59))
        return false;
    if (has_time) *has_time = ht;
    if (year) *year = y;
    if (month) *month = mo;
    if (day) *day = d;
    if (hour) *hour = h;
    if (minute) *minute = mi;
    if (second) *second = se;
    return true;
}

bool is_valid_iso_date(const char *s) {
    bool has_time = false;
    return parse_iso_datetime(s, &has_time, NULL, NULL, NULL, NULL, NULL, NULL)
           && !has_time;
}

bool is_valid_iso_time(const char *s) {
    if (s == NULL) return false;
    int h = 0, mi = 0, se = 0, consumed = 0;
    if (sscanf(s, "%2d:%2d:%2d%n", &h, &mi, &se, &consumed) != 3) return false;
    if (s[consumed] != '\0') return false;
    return h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && se >= 0 && se <= 59;
}

bool is_valid_iso_timestamp(const char *s) {
    return parse_iso_datetime(s, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

bool is_valid_extract_field(const char *f) {
    return str_ieq(f, "YEAR") || str_ieq(f, "MONTH") || str_ieq(f, "DAY") ||
           str_ieq(f, "HOUR") || str_ieq(f, "MINUTE") || str_ieq(f, "SECOND") ||
           str_ieq(f, "QUARTER");
}

EvalResult eval_datetime_value(int kind, QArena *arena) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char buf[32];
    if (lt == NULL) return eval_result_null();
    switch (kind) {
        case DT_CURRENT_DATE:
            strftime(buf, sizeof(buf), "%Y-%m-%d", lt);
            break;
        case DT_CURRENT_TIME:
        case DT_LOCALTIME:
            strftime(buf, sizeof(buf), "%H:%M:%S", lt);
            break;
        default:
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
            break;
    }
    const char *dup = qarena_strdup(arena, buf);
    return eval_result_str(dup ? dup : "");
}

EvalResult eval_date_part(const char *field, const EvalResult *v) {
    if (v->is_null) return eval_result_null();
    const char *s = v->is_numeric ? NULL : v->str_val;
    if (s == NULL) return eval_result_null();
    bool has_time = false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!parse_iso_datetime(s, &has_time, &y, &mo, &d, &h, &mi, &se))
        return eval_result_null();
    if (str_ieq(field, "YEAR")) return eval_result_num((double)y);
    if (str_ieq(field, "MONTH")) return eval_result_num((double)mo);
    if (str_ieq(field, "DAY")) return eval_result_num((double)d);
    if (str_ieq(field, "QUARTER")) return eval_result_num((double)((mo - 1) / 3 + 1));
    if (str_ieq(field, "HOUR")) return eval_result_num((double)(has_time ? h : 0));
    if (str_ieq(field, "MINUTE")) return eval_result_num((double)(has_time ? mi : 0));
    if (str_ieq(field, "SECOND")) return eval_result_num((double)(has_time ? se : 0));
    return eval_result_null();
}

/* Days since the civil epoch (1970-01-01), from Howard Hinnant's
   days_from_civil: correct for all valid ISO dates, leap years included. */
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static bool days_of(const EvalResult *v, long long *out) {
    if (v->is_null) return false;
    const char *s = v->is_numeric ? NULL : v->str_val;
    if (s == NULL) return false;
    int y = 0, mo = 0, d = 0;
    if (!parse_iso_datetime(s, NULL, &y, &mo, &d, NULL, NULL, NULL))
        return false;
    *out = days_from_civil(y, mo, d);
    return true;
}

EvalResult eval_datediff(const EvalResult *a, const EvalResult *b) {
    long long da = 0, db = 0;
    if (!days_of(a, &da) || !days_of(b, &db)) return eval_result_null();
    return eval_result_num((double)(da - db));
}
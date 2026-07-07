#include "csv_parser.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static inline uint16_t neon_movemask(uint8x16_t cmp) {
    uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
    uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
    const uint64_t magic = 0x000103070f1f3f80ULL;
    lo = (lo * magic) >> 56;
    hi = (hi * magic) >> 56;
    return (uint16_t)((hi << 8) | lo);
}
#endif

static void init_field_array(FieldArray *arr, Arena *arena, size_t initial_capacity) {
    void *ptr;
    ArenaResult result = arena_alloc(arena, sizeof(char*) * initial_capacity, &ptr);
    if (result != ARENA_OK) {
        arr->fields = NULL;
        arr->count = 0;
        arr->capacity = 0;
        return;
    }
    arr->fields = (char**)ptr;
    arr->count = 0;
    arr->capacity = initial_capacity;
}

static bool grow_field_array(FieldArray *arr, Arena *arena) {
    size_t new_capacity = arr->capacity * 2;
    void *ptr;
    ArenaResult result = arena_alloc(arena, sizeof(char*) * new_capacity, &ptr);
    if (result != ARENA_OK) {
        return false;
    }
    char **new_fields = (char**)ptr;
    memcpy(new_fields, arr->fields, sizeof(char*) * arr->count);
    arr->fields = new_fields;
    arr->capacity = new_capacity;
    return true;
}

static bool add_field(FieldArray *arr, const char *start, size_t len, Arena *arena) {
    if (arr->count >= arr->capacity) {
        if (!grow_field_array(arr, arena)) {
            return false;
        }
    }

    while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) {
        len--;
    }

    void *ptr;
    ArenaResult result = arena_alloc(arena, len + 1, &ptr);
    if (result != ARENA_OK) {
        return false;
    }
    char *field = (char*)ptr;
    memcpy(field, start, len);
    field[len] = '\0';
    arr->fields[arr->count++] = field;
    return true;
}

static bool add_quoted_field(FieldArray *arr, const char *start, size_t len, Arena *arena, char enclosure) {
    if (arr->count >= arr->capacity) {
        if (!grow_field_array(arr, arena)) {
            return false;
        }
    }

    void *ptr;
    ArenaResult result = arena_alloc(arena, len + 1, &ptr);
    if (result != ARENA_OK) {
        return false;
    }

    char *field = (char*)ptr;
    size_t write_pos = 0;
    size_t i = 0;

#if defined(__AVX2__)
    const __m256i encl_v = _mm256_set1_epi8(enclosure);
    while (i + 32 <= len) {
        __m256i data = _mm256_loadu_si256((const __m256i*)(start + i));
        int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(data, encl_v));
        if (mask == 0) {
            memcpy(field + write_pos, start + i, 32);
            write_pos += 32;
            i += 32;
        } else {
            size_t end = i + 32 > len ? len : i + 32;
            for (; i < end; i++) {
                if (start[i] == enclosure && i + 1 < len && start[i + 1] == enclosure) {
                    field[write_pos++] = enclosure;
                    i++;
                } else {
                    field[write_pos++] = start[i];
                }
            }
        }
    }
#elif defined(__SSE2__)
    const __m128i encl_v = _mm_set1_epi8(enclosure);
    while (i + 16 <= len) {
        __m128i data = _mm_loadu_si128((const __m128i*)(start + i));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(data, encl_v));
        if (mask == 0) {
            memcpy(field + write_pos, start + i, 16);
            write_pos += 16;
            i += 16;
        } else {
            size_t end = i + 16 > len ? len : i + 16;
            for (; i < end; i++) {
                if (start[i] == enclosure && i + 1 < len && start[i + 1] == enclosure) {
                    field[write_pos++] = enclosure;
                    i++;
                } else {
                    field[write_pos++] = start[i];
                }
            }
        }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8x16_t encl_v = vdupq_n_u8((uint8_t)enclosure);
    while (i + 16 <= len) {
        uint8x16_t data = vld1q_u8((const uint8_t*)(start + i));
        uint16_t mask = neon_movemask(vceqq_u8(data, encl_v));
        if (mask == 0) {
            memcpy(field + write_pos, start + i, 16);
            write_pos += 16;
            i += 16;
        } else {
            size_t end = i + 16 > len ? len : i + 16;
            for (; i < end; i++) {
                if (start[i] == enclosure && i + 1 < len && start[i + 1] == enclosure) {
                    field[write_pos++] = enclosure;
                    i++;
                } else {
                    field[write_pos++] = start[i];
                }
            }
        }
    }
#endif

    for (; i < len; i++) {
        if (start[i] == enclosure && i + 1 < len && start[i + 1] == enclosure) {
            field[write_pos++] = enclosure;
            i++;
        } else {
            field[write_pos++] = start[i];
        }
    }

    field[write_pos] = '\0';
    arr->fields[arr->count++] = field;
    return true;
}

static CSVParseResult
csv_parse_line_inplace_scalar(const char *line, Arena *arena, const CSVConfig *config, int line_number) {
    CSVParseResult result = {0};
    result.success = true;
    result.error = NULL;
    result.error_line = line_number;
    result.error_column = 0;

    size_t len = strlen(line);

    if (!line || !arena || !config) {
        result.success = false;
        result.error = "Invalid arguments";
        return result;
    }

    init_field_array(&result.fields, arena, 16);
    if (!result.fields.fields) {
        result.success = false;
        result.error = "Failed to allocate field array";
        return result;
    }

    ParseState state = FIELD_START;
    const char *field_start = line;
    size_t field_len = 0;
    size_t pos = 0;

    while (pos < len) {
        char c = line[pos];
        
        switch (state) {
            case FIELD_START:
                if (c == config->enclosure) {
                    state = QUOTED_FIELD;
                    field_start = &line[pos + 1];
                    field_len = 0;
                } else if (c == config->delimiter) {
                    if (!add_field(&result.fields, "", 0, arena)) {
                        result.success = false;
                        result.error = "Memory allocation failed";
                        result.error_column = pos;
                        return result;
                    }
                    field_start = &line[pos + 1];
                    field_len = 0;
                } else {
                    state = UNQUOTED_FIELD;
                    field_start = &line[pos];
                    field_len = 1;
                }
                break;

            case UNQUOTED_FIELD:
                if (c == config->delimiter) {
                    if (!add_field(&result.fields, field_start, field_len, arena)) {
                        result.success = false;
                        result.error = "Memory allocation failed";
                        result.error_column = pos;
                        return result;
                    }
                    state = FIELD_START;
                    field_start = &line[pos + 1];
                    field_len = 0;
                } else {
                    field_len++;
                }
                break;

            case QUOTED_FIELD:
                if (c == config->enclosure) {
                    if (pos + 1 < len && line[pos + 1] == config->enclosure) {
                        field_len += 2;
                        pos++;
                    } else {
                        state = FIELD_END;
                    }
                } else {
                    field_len++;
                }
                break;

            case FIELD_END:
                if (c == config->delimiter) {
                    if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                        result.success = false;
                        result.error = "Memory allocation failed";
                        result.error_column = pos;
                        return result;
                    }
                    state = FIELD_START;
                    field_start = &line[pos + 1];
                    field_len = 0;
                } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    result.success = false;
                    result.error = "Expected delimiter after quoted field";
                    result.error_column = pos;
                    return result;
                }
                break;

            default:
                result.success = false;
                result.error = "Invalid parser state";
                result.error_column = pos;
                return result;
        }
        pos++;
    }

    if (state == QUOTED_FIELD) {
        result.success = false;
        result.error = "Unclosed quote";
        result.error_column = pos;
        return result;
    }

    if (field_len > 0 || state == FIELD_START) {
        if (state == FIELD_END) {
            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        } else {
            if (!add_field(&result.fields, field_start, field_len, arena)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        }
    }

    return result;
}

#if defined(__AVX2__)
static CSVParseResult csv_parse_line_inplace_simd(const char *line, Arena *arena,
                                                   const CSVConfig *config, int line_number) {
    CSVParseResult result = {0};
    result.success = true;
    result.error = NULL;
    result.error_line = line_number;
    result.error_column = 0;

    size_t len = strlen(line);

    if (!line || !arena || !config) {
        result.success = false;
        result.error = "Invalid arguments";
        return result;
    }

    init_field_array(&result.fields, arena, 16);
    if (!result.fields.fields) {
        result.success = false;
        result.error = "Failed to allocate field array";
        return result;
    }

    ParseState state = FIELD_START;
    const char *field_start = line;
    size_t field_len = 0;
    size_t pos = 0;

    const __m256i delim_v = _mm256_set1_epi8(config->delimiter);
    const __m256i encl_v  = _mm256_set1_epi8(config->enclosure);

    while (pos < len) {
        size_t remaining = len - pos;
        if (remaining >= 32 && state != FIELD_END) {
            __m256i data = _mm256_loadu_si256((const __m256i*)(line + pos));
            __m256i ed = _mm256_cmpeq_epi8(data, delim_v);
            __m256i ee = _mm256_cmpeq_epi8(data, encl_v);
            int dm = _mm256_movemask_epi8(ed);
            int em = _mm256_movemask_epi8(ee);
            int both = dm | em;

            if (both == 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos;
                }
                field_len += 32;
                pos += 32;
                continue;
            }

            int prev = 0;
            while (both) {
                int bit = __builtin_ctz(both);
                int reg = bit - prev;
                if (reg > 0) {
                    if (state == FIELD_START) {
                        state = UNQUOTED_FIELD;
                        field_start = line + pos + prev;
                        field_len = reg;
                    } else {
                        field_len += reg;
                    }
                }

                char c = line[pos + bit];
                int is_delim = (dm >> bit) & 1;
                int is_encl  = (em >> bit) & 1;

                switch (state) {
                    case FIELD_START:
                        if (is_encl) {
                            state = QUOTED_FIELD;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_delim) {
                            if (!add_field(&result.fields, "", 0, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        }
                        break;

                    case UNQUOTED_FIELD:
                        if (is_delim) {
                            if (!add_field(&result.fields, field_start, field_len, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_encl) {
                            field_len++;
                        }
                        break;

                    case QUOTED_FIELD:
                        if (is_encl) {
                            size_t abs_pos = pos + bit;
                            if (abs_pos + 1 < len && line[abs_pos + 1] == config->enclosure) {
                                field_len += 2;
                                if (bit + 1 < 32) {
                                    both &= ~(1 << (bit + 1));
                                }
                                prev = bit + 2;
                                both &= both - 1;
                                continue;
                            } else {
                                state = FIELD_END;
                            }
                        } else if (is_delim) {
                            field_len++;
                        }
                        break;

                    case FIELD_END:
                        if (is_delim) {
                            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        } else {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + bit;
                            return result;
                        }
                        break;

                    default:
                        result.success = false;
                        result.error = "Invalid parser state";
                        result.error_column = pos + bit;
                        return result;
                }

                prev = bit + 1;
                both &= both - 1;
            }

            int trail = 32 - prev;
            if (trail > 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos + prev;
                }
                if (state != FIELD_END) {
                    field_len += trail;
                } else {
                    for (int i = 0; i < trail; i++) {
                        char tc = line[pos + prev + i];
                        if (tc != ' ' && tc != '\t' && tc != '\r' && tc != '\n') {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + prev + i;
                            return result;
                        }
                    }
                }
            }

            pos += 32;
            continue;
        }

        while (pos < len) {
            char c = line[pos];
            switch (state) {
                case FIELD_START:
                    if (c == config->enclosure) {
                        state = QUOTED_FIELD;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c == config->delimiter) {
                        if (!add_field(&result.fields, "", 0, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        state = UNQUOTED_FIELD;
                        field_start = &line[pos];
                        field_len = 1;
                    }
                    break;

                case UNQUOTED_FIELD:
                    if (c == config->delimiter) {
                        if (!add_field(&result.fields, field_start, field_len, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        field_len++;
                    }
                    break;

                case QUOTED_FIELD:
                    if (c == config->enclosure) {
                        if (pos + 1 < len && line[pos + 1] == config->enclosure) {
                            field_len += 2;
                            pos++;
                        } else {
                            state = FIELD_END;
                        }
                    } else {
                        field_len++;
                    }
                    break;

                case FIELD_END:
                    if (c == config->delimiter) {
                        if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                        result.success = false;
                        result.error = "Expected delimiter after quoted field";
                        result.error_column = pos;
                        return result;
                    }
                    break;

                default:
                    result.success = false;
                    result.error = "Invalid parser state";
                    result.error_column = pos;
                    return result;
            }
            pos++;
        }
    }

    if (state == QUOTED_FIELD) {
        result.success = false;
        result.error = "Unclosed quote";
        result.error_column = pos;
        return result;
    }

    if (field_len > 0 || state == FIELD_START) {
        if (state == FIELD_END) {
            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        } else {
            if (!add_field(&result.fields, field_start, field_len, arena)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        }
    }

    return result;
}

#elif defined(__SSE2__)
static CSVParseResult csv_parse_line_inplace_simd(const char *line, Arena *arena,
                                                   const CSVConfig *config, int line_number) {
    size_t len = strlen(line);
    CSVParseResult result = {0};
    result.success = true;
    result.error = NULL;
    result.error_line = line_number;
    result.error_column = 0;

    if (!line || !arena || !config) {
        result.success = false;
        result.error = "Invalid arguments";
        return result;
    }

    init_field_array(&result.fields, arena, 16);
    if (!result.fields.fields) {
        result.success = false;
        result.error = "Failed to allocate field array";
        return result;
    }

    ParseState state = FIELD_START;
    const char *field_start = line;
    size_t field_len = 0;
    size_t pos = 0;

    const __m128i delim_v = _mm_set1_epi8(config->delimiter);
    const __m128i encl_v  = _mm_set1_epi8(config->enclosure);

    while (pos < len) {
        size_t remaining = len - pos;
        if (remaining >= 16 && state != FIELD_END) {
            __m128i data = _mm_loadu_si128((const __m128i*)(line + pos));
            __m128i ed = _mm_cmpeq_epi8(data, delim_v);
            __m128i ee = _mm_cmpeq_epi8(data, encl_v);
            int dm = _mm_movemask_epi8(ed);
            int em = _mm_movemask_epi8(ee);
            int both = dm | em;

            if (both == 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos;
                }
                field_len += 16;
                pos += 16;
                continue;
            }

            int prev = 0;
            while (both) {
                int bit = __builtin_ctz(both);
                int reg = bit - prev;
                if (reg > 0) {
                    if (state == FIELD_START) {
                        state = UNQUOTED_FIELD;
                        field_start = line + pos + prev;
                        field_len = reg;
                    } else {
                        field_len += reg;
                    }
                }

                char c = line[pos + bit];
                int is_delim = (dm >> bit) & 1;
                int is_encl  = (em >> bit) & 1;

                switch (state) {
                    case FIELD_START:
                        if (is_encl) {
                            state = QUOTED_FIELD;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_delim) {
                            if (!add_field(&result.fields, "", 0, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        }
                        break;

                    case UNQUOTED_FIELD:
                        if (is_delim) {
                            if (!add_field(&result.fields, field_start, field_len, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_encl) {
                            field_len++;
                        }
                        break;

                    case QUOTED_FIELD:
                        if (is_encl) {
                            size_t abs_pos = pos + bit;
                            if (abs_pos + 1 < len && line[abs_pos + 1] == config->enclosure) {
                                field_len += 2;
                                if (bit + 1 < 16) {
                                    both &= ~(1 << (bit + 1));
                                }
                                prev = bit + 2;
                                both &= both - 1;
                                continue;
                            } else {
                                state = FIELD_END;
                            }
                        } else if (is_delim) {
                            field_len++;
                        }
                        break;

                    case FIELD_END:
                        if (is_delim) {
                            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        } else {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + bit;
                            return result;
                        }
                        break;

                    default:
                        result.success = false;
                        result.error = "Invalid parser state";
                        result.error_column = pos + bit;
                        return result;
                }

                prev = bit + 1;
                both &= both - 1;
            }

            int trail = 16 - prev;
            if (trail > 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos + prev;
                }
                if (state != FIELD_END) {
                    field_len += trail;
                } else {
                    for (int i = 0; i < trail; i++) {
                        char tc = line[pos + prev + i];
                        if (tc != ' ' && tc != '\t' && tc != '\r' && tc != '\n') {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + prev + i;
                            return result;
                        }
                    }
                }
            }

            pos += 16;
            continue;
        }

        while (pos < len) {
            char c = line[pos];
            switch (state) {
                case FIELD_START:
                    if (c == config->enclosure) {
                        state = QUOTED_FIELD;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c == config->delimiter) {
                        if (!add_field(&result.fields, "", 0, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        state = UNQUOTED_FIELD;
                        field_start = &line[pos];
                        field_len = 1;
                    }
                    break;

                case UNQUOTED_FIELD:
                    if (c == config->delimiter) {
                        if (!add_field(&result.fields, field_start, field_len, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        field_len++;
                    }
                    break;

                case QUOTED_FIELD:
                    if (c == config->enclosure) {
                        if (pos + 1 < len && line[pos + 1] == config->enclosure) {
                            field_len += 2;
                            pos++;
                        } else {
                            state = FIELD_END;
                        }
                    } else {
                        field_len++;
                    }
                    break;

                case FIELD_END:
                    if (c == config->delimiter) {
                        if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                        result.success = false;
                        result.error = "Expected delimiter after quoted field";
                        result.error_column = pos;
                        return result;
                    }
                    break;

                default:
                    result.success = false;
                    result.error = "Invalid parser state";
                    result.error_column = pos;
                    return result;
            }
            pos++;
        }
    }

    if (state == QUOTED_FIELD) {
        result.success = false;
        result.error = "Unclosed quote";
        result.error_column = pos;
        return result;
    }

    if (field_len > 0 || state == FIELD_START) {
        if (state == FIELD_END) {
            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        } else {
            if (!add_field(&result.fields, field_start, field_len, arena)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        }
    }

    return result;
}

#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
static CSVParseResult csv_parse_line_inplace_simd(const char *line, Arena *arena,
                                                   const CSVConfig *config, int line_number) {
    CSVParseResult result = {0};
    result.success = true;
    result.error = NULL;
    result.error_line = line_number;
    result.error_column = 0;

    size_t len = strlen(line);

    if (!line || !arena || !config) {
        result.success = false;
        result.error = "Invalid arguments";
        return result;
    }

    init_field_array(&result.fields, arena, 16);
    if (!result.fields.fields) {
        result.success = false;
        result.error = "Failed to allocate field array";
        return result;
    }

    ParseState state = FIELD_START;
    const char *field_start = line;
    size_t field_len = 0;
    size_t pos = 0;

    const uint8x16_t delim_v = vdupq_n_u8((uint8_t)config->delimiter);
    const uint8x16_t encl_v  = vdupq_n_u8((uint8_t)config->enclosure);

    while (pos < len) {
        size_t remaining = len - pos;
        if (remaining >= 16 && state != FIELD_END) {
            uint8x16_t data = vld1q_u8((const uint8_t*)(line + pos));
            uint8x16_t ed = vceqq_u8(data, delim_v);
            uint8x16_t ee = vceqq_u8(data, encl_v);
            uint8x16_t special = vorrq_u8(ed, ee);
            uint16_t both = neon_movemask(special);
            int dm = neon_movemask(ed);
            int em = neon_movemask(ee);

            if (both == 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos;
                }
                field_len += 16;
                pos += 16;
                continue;
            }

            int prev = 0;
            uint16_t remaining_bits = both;
            while (remaining_bits) {
                int bit = __builtin_ctz(remaining_bits);
                int reg = bit - prev;
                if (reg > 0) {
                    if (state == FIELD_START) {
                        state = UNQUOTED_FIELD;
                        field_start = line + pos + prev;
                        field_len = reg;
                    } else {
                        field_len += reg;
                    }
                }

                char c = line[pos + bit];
                int is_delim = (dm >> bit) & 1;
                int is_encl  = (em >> bit) & 1;

                switch (state) {
                    case FIELD_START:
                        if (is_encl) {
                            state = QUOTED_FIELD;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_delim) {
                            if (!add_field(&result.fields, "", 0, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        }
                        break;

                    case UNQUOTED_FIELD:
                        if (is_delim) {
                            if (!add_field(&result.fields, field_start, field_len, arena)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (is_encl) {
                            field_len++;
                        }
                        break;

                    case QUOTED_FIELD:
                        if (is_encl) {
                            size_t abs_pos = pos + bit;
                            if (abs_pos + 1 < len && line[abs_pos + 1] == config->enclosure) {
                                field_len += 2;
                                if (bit + 1 < 16) {
                                    remaining_bits &= ~(uint16_t)(1 << (bit + 1));
                                }
                                prev = bit + 2;
                                remaining_bits &= remaining_bits - 1;
                                continue;
                            } else {
                                state = FIELD_END;
                            }
                        } else if (is_delim) {
                            field_len++;
                        }
                        break;

                    case FIELD_END:
                        if (is_delim) {
                            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                                result.success = false;
                                result.error = "Memory allocation failed";
                                result.error_column = pos + bit;
                                return result;
                            }
                            state = FIELD_START;
                            field_start = &line[pos + bit + 1];
                            field_len = 0;
                        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        } else {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + bit;
                            return result;
                        }
                        break;

                    default:
                        result.success = false;
                        result.error = "Invalid parser state";
                        result.error_column = pos + bit;
                        return result;
                }

                prev = bit + 1;
                remaining_bits &= remaining_bits - 1;
            }

            int trail = 16 - prev;
            if (trail > 0) {
                if (state == FIELD_START) {
                    state = UNQUOTED_FIELD;
                    field_start = line + pos + prev;
                }
                if (state != FIELD_END) {
                    field_len += trail;
                } else {
                    for (int i = 0; i < trail; i++) {
                        char tc = line[pos + prev + i];
                        if (tc != ' ' && tc != '\t' && tc != '\r' && tc != '\n') {
                            result.success = false;
                            result.error = "Expected delimiter after quoted field";
                            result.error_column = pos + prev + i;
                            return result;
                        }
                    }
                }
            }

            pos += 16;
            continue;
        }

        while (pos < len) {
            char c = line[pos];
            switch (state) {
                case FIELD_START:
                    if (c == config->enclosure) {
                        state = QUOTED_FIELD;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c == config->delimiter) {
                        if (!add_field(&result.fields, "", 0, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        state = UNQUOTED_FIELD;
                        field_start = &line[pos];
                        field_len = 1;
                    }
                    break;

                case UNQUOTED_FIELD:
                    if (c == config->delimiter) {
                        if (!add_field(&result.fields, field_start, field_len, arena)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else {
                        field_len++;
                    }
                    break;

                case QUOTED_FIELD:
                    if (c == config->enclosure) {
                        if (pos + 1 < len && line[pos + 1] == config->enclosure) {
                            field_len += 2;
                            pos++;
                        } else {
                            state = FIELD_END;
                        }
                    } else {
                        field_len++;
                    }
                    break;

                case FIELD_END:
                    if (c == config->delimiter) {
                        if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                            result.success = false;
                            result.error = "Memory allocation failed";
                            result.error_column = pos;
                            return result;
                        }
                        state = FIELD_START;
                        field_start = &line[pos + 1];
                        field_len = 0;
                    } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                        result.success = false;
                        result.error = "Expected delimiter after quoted field";
                        result.error_column = pos;
                        return result;
                    }
                    break;

                default:
                    result.success = false;
                    result.error = "Invalid parser state";
                    result.error_column = pos;
                    return result;
            }
            pos++;
        }
    }

    if (state == QUOTED_FIELD) {
        result.success = false;
        result.error = "Unclosed quote";
        result.error_column = pos;
        return result;
    }

    if (field_len > 0 || state == FIELD_START) {
        if (state == FIELD_END) {
            if (!add_quoted_field(&result.fields, field_start, field_len, arena, config->enclosure)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        } else {
            if (!add_field(&result.fields, field_start, field_len, arena)) {
                result.success = false;
                result.error = "Memory allocation failed";
                return result;
            }
        }
    }

    return result;
}

#endif

CSVParseResult csv_parse_line_inplace(const char *line, Arena *arena,
                                       const CSVConfig *config, int line_number) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
    return csv_parse_line_inplace_simd(line, arena, config, line_number);
#else
    return csv_parse_line_inplace_scalar(line, arena, config, line_number);
#endif
}

char* read_full_record(FILE *file, Arena *arena) {
    if (!file || !arena) return NULL;

    long start = ftell(file);
    if (start == -1) return NULL;

    char buf[4096];
    size_t nread = fread(buf, 1, sizeof(buf), file);

    if (nread == 0) return NULL;

    bool in_quotes = false;
    size_t content_end = 0;
    size_t nl_len = 0;

    while (content_end < nread) {
        char c = buf[content_end];

        if (c == '"') {
            if (in_quotes && content_end + 1 < nread &&
                buf[content_end + 1] == '"') {
                content_end += 2;
                continue;
            }
            in_quotes = !in_quotes;
            content_end++;
        } else if ((c == '\n' || c == '\r') && !in_quotes) {
            nl_len = 1;
            if (c == '\r' && content_end + 1 < nread &&
                buf[content_end + 1] == '\n') {
                nl_len = 2;
            }
            break;
        } else {
            content_end++;
        }
    }

    if (nl_len == 0 && nread == sizeof(buf)) {
        size_t capacity = 4096;
        void *arena_ptr;
        ArenaResult res = arena_alloc(arena, capacity, &arena_ptr);
        if (res != ARENA_OK) return NULL;
        char *record = (char*)arena_ptr;
        memcpy(record, buf, nread);
        size_t record_len = nread;

        while (1) {
            size_t more = fread(buf, 1, sizeof(buf), file);
            if (more == 0) break;

            while (record_len + more > capacity) {
                size_t new_cap = capacity * 2;
                void *new_ptr;
                ArenaResult grow = arena_alloc(arena, new_cap, &new_ptr);
                if (grow != ARENA_OK) return NULL;
                char *new_rec = (char*)new_ptr;
                memcpy(new_rec, record, record_len);
                record = new_rec;
                capacity = new_cap;
            }

            size_t i = 0;
            while (i < more) {
                char c = buf[i];

                if (c == '"') {
                    if (in_quotes && i + 1 < more && buf[i + 1] == '"') {
                        record[record_len++] = '"';
                        record[record_len++] = '"';
                        i += 2;
                        continue;
                    }
                    in_quotes = !in_quotes;
                    record[record_len++] = '"';
                    i++;
                } else if ((c == '\n' || c == '\r') && !in_quotes) {
                    nl_len = 1;
                    if (c == '\r' && i + 1 < more && buf[i + 1] == '\n') {
                        nl_len = 2;
                    }
                    content_end = record_len + i;

                    record[content_end] = '\0';
                    // if (out_len) *out_len = content_end;
                    long after = start + content_end + nl_len;
                    fseek(file, after, SEEK_SET);
                    return record;
                } else {
                    record[record_len++] = c;
                    i++;
                }
            }
        }

        record[record_len] = '\0';
        // if (out_len) *out_len = record_len;
        return record;
    }

    void *arena_ptr;
    ArenaResult res = arena_alloc(arena, content_end + 1, &arena_ptr);
    if (res != ARENA_OK) return NULL;
    char *record = (char*)arena_ptr;
    memcpy(record, buf, content_end);
    record[content_end] = '\0';
    // if (out_len) *out_len = content_end;

    long after = start + content_end + nl_len;
    fseek(file, after, SEEK_SET);

    return record;
} 
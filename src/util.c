#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return true;
    size_t next = sb->cap ? sb->cap : 256;
    while (next < sb->len + extra + 1) next *= 2;
    char *grown = (char *)realloc(sb->data, next);
    if (!grown) return false;
    sb->data = grown;
    sb->cap = next;
    return true;
}

void sb_init(StrBuf *sb, size_t initial) {
    memset(sb, 0, sizeof(*sb));
    if (initial < 32) initial = 32;
    sb->data = (char *)malloc(initial);
    if (sb->data) {
        sb->cap = initial;
        sb->data[0] = '\0';
    }
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}

bool sb_append_n(StrBuf *sb, const char *text, size_t len) {
    if (!sb_reserve(sb, len)) return false;
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

bool sb_append(StrBuf *sb, const char *text) {
    return sb_append_n(sb, text, strlen(text));
}

bool sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0 || !sb_reserve(sb, (size_t)needed)) {
        va_end(args);
        return false;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    va_end(args);
    sb->len += (size_t)needed;
    return true;
}

bool sb_json_string(StrBuf *sb, const char *text) {
    if (!sb_append_n(sb, "\"", 1)) return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        switch (*p) {
            case '\"': if (!sb_append(sb, "\\\"")) return false; break;
            case '\\': if (!sb_append(sb, "\\\\")) return false; break;
            case '\b': if (!sb_append(sb, "\\b")) return false; break;
            case '\f': if (!sb_append(sb, "\\f")) return false; break;
            case '\n': if (!sb_append(sb, "\\n")) return false; break;
            case '\r': if (!sb_append(sb, "\\r")) return false; break;
            case '\t': if (!sb_append(sb, "\\t")) return false; break;
            default:
                if (*p < 0x20) {
                    if (!sb_printf(sb, "\\u%04x", *p)) return false;
                } else if (!sb_append_n(sb, (const char *)p, 1)) return false;
        }
        ++p;
    }
    return sb_append_n(sb, "\"", 1);
}

static void utf8_codepoint(StrBuf *sb, uint32_t cp) {
    char bytes[4];
    size_t n = 0;
    if (cp <= 0x7F) bytes[n++] = (char)cp;
    else if (cp <= 0x7FF) {
        bytes[n++] = (char)(0xC0 | (cp >> 6));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        bytes[n++] = (char)(0xE0 | (cp >> 12));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        bytes[n++] = (char)(0xF0 | (cp >> 18));
        bytes[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    }
    sb_append_n(sb, bytes, n);
}

static int hex4(const char *p) {
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c >= '0' && c <= '9') value = value * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = value * 16 + c - 'A' + 10;
        else return -1;
    }
    return value;
}

bool json_token_string(const char *token, size_t length, char *out, size_t out_size) {
    if (!token || length < 2 || token[0] != '\"') return false;
    StrBuf sb;
    sb_init(&sb, length + 1);
    size_t i = 1;
    size_t end = length;
    if (token[length - 1] == '\"') end = length - 1;
    while (i < end) {
        unsigned char c = (unsigned char)token[i++];
        if (c != '\\') {
            sb_append_n(&sb, (const char *)&c, 1);
            continue;
        }
        if (i >= end) break;
        c = (unsigned char)token[i++];
        switch (c) {
            case '\"': case '\\': case '/': sb_append_n(&sb, (const char *)&c, 1); break;
            case 'b': sb_append_n(&sb, "\b", 1); break;
            case 'f': sb_append_n(&sb, "\f", 1); break;
            case 'n': sb_append_n(&sb, "\n", 1); break;
            case 'r': sb_append_n(&sb, "\r", 1); break;
            case 't': sb_append_n(&sb, "\t", 1); break;
            case 'u': {
                if (i + 4 > end) break;
                int first = hex4(token + i);
                i += 4;
                if (first < 0) break;
                uint32_t cp = (uint32_t)first;
                if (first >= 0xD800 && first <= 0xDBFF && i + 6 <= end &&
                    token[i] == '\\' && token[i + 1] == 'u') {
                    int second = hex4(token + i + 2);
                    if (second >= 0xDC00 && second <= 0xDFFF) {
                        cp = 0x10000u + (((uint32_t)first - 0xD800u) << 10) +
                             ((uint32_t)second - 0xDC00u);
                        i += 6;
                    }
                }
                utf8_codepoint(&sb, cp);
                break;
            }
            default: sb_append_n(&sb, (const char *)&c, 1); break;
        }
    }
    if (!sb.data || out_size == 0) { sb_free(&sb); return false; }
    size_t copy = sb.len < out_size - 1 ? sb.len : out_size - 1;
    memcpy(out, sb.data, copy);
    out[copy] = '\0';
    sb_free(&sb);
    return true;
}

const char *json_find_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += strlen(pattern);
        while (isspace((unsigned char)*p)) ++p;
        if (*p++ != ':') continue;
        while (isspace((unsigned char)*p)) ++p;
        return p;
    }
    return NULL;
}

static size_t json_value_length(const char *p) {
    if (!p) return 0;
    if (*p == '\"') {
        const char *q = p + 1;
        bool escape = false;
        while (*q) {
            if (!escape && *q == '\"') return (size_t)(q - p + 1);
            if (!escape && *q == '\\') escape = true;
            else escape = false;
            ++q;
        }
        return 0;
    }
    if (*p == '[' || *p == '{') {
        char open = *p, close = open == '[' ? ']' : '}';
        int depth = 0;
        bool string = false, escape = false;
        for (const char *q = p; *q; ++q) {
            if (string) {
                if (!escape && *q == '\"') string = false;
                if (!escape && *q == '\\') escape = true; else escape = false;
                continue;
            }
            if (*q == '\"') { string = true; continue; }
            if (*q == open) ++depth;
            else if (*q == close && --depth == 0) return (size_t)(q - p + 1);
        }
        return 0;
    }
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ']' && !isspace((unsigned char)*q)) ++q;
    return (size_t)(q - p);
}

bool json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *value = json_find_value(json, key);
    size_t len = json_value_length(value);
    return json_token_string(value, len, out, out_size);
}

long long json_get_integer(const char *json, const char *key, long long fallback) {
    const char *value = json_find_value(json, key);
    if (!value) return fallback;
    char *end = NULL;
    long long result = strtoll(value, &end, 10);
    return end == value ? fallback : result;
}

bool json_get_boolean(const char *json, const char *key, bool fallback) {
    const char *value = json_find_value(json, key);
    if (!value) return fallback;
    if (strncmp(value, "true", 4) == 0) return true;
    if (strncmp(value, "false", 5) == 0) return false;
    return fallback;
}

bool json_array_element(const char *array, int index, const char **start, size_t *length) {
    if (!array || *array != '[' || index < 0) return false;
    const char *p = array + 1;
    int current = 0;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') ++p;
        if (*p == ']') return false;
        size_t len = json_value_length(p);
        if (!len) return false;
        if (current == index) {
            *start = p;
            *length = len;
            return true;
        }
        p += len;
        ++current;
    }
    return false;
}

char *read_entire_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) { fclose(file); return NULL; }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) { fclose(file); return NULL; }
    size_t read = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read] = '\0';
    if (size_out) *size_out = read;
    return data;
}

bool write_entire_file(const char *path, const char *data, size_t length) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, length, file) == length;
    fclose(file);
    return ok;
}


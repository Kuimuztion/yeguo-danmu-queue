#ifndef BYCUTE_UTIL_H
#define BYCUTE_UTIL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb, size_t initial);
void sb_free(StrBuf *sb);
bool sb_append(StrBuf *sb, const char *text);
bool sb_append_n(StrBuf *sb, const char *text, size_t len);
bool sb_printf(StrBuf *sb, const char *fmt, ...);
bool sb_json_string(StrBuf *sb, const char *text);

const char *json_find_value(const char *json, const char *key);
bool json_get_string(const char *json, const char *key, char *out, size_t out_size);
long long json_get_integer(const char *json, const char *key, long long fallback);
bool json_get_boolean(const char *json, const char *key, bool fallback);
bool json_array_element(const char *array, int index, const char **start, size_t *length);
bool json_token_string(const char *token, size_t length, char *out, size_t out_size);
char *read_entire_file(const char *path, size_t *size_out);
bool write_entire_file(const char *path, const char *data, size_t length);

#endif


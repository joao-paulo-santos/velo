#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct json_parser {
	const char *pos;
	const char *error;
} json_parser_t;

void json_parser_init(json_parser_t *p, const char *json);

bool json_skip_ws(json_parser_t *p);
bool json_peek_char(json_parser_t *p, char c);
bool json_expect_char(json_parser_t *p, char c);
bool json_skip_value(json_parser_t *p);

bool json_parse_string(json_parser_t *p, char *out, size_t max_len);
bool json_parse_bool(json_parser_t *p, bool *out);
bool json_parse_number(json_parser_t *p, double *out);
bool json_parse_null(json_parser_t *p);

bool json_object_begin(json_parser_t *p);
bool json_object_next(json_parser_t *p, char *key, size_t key_max, bool *has_more);
bool json_object_end(json_parser_t *p);

bool json_array_begin(json_parser_t *p);
bool json_array_next(json_parser_t *p, bool *has_more);
bool json_array_end(json_parser_t *p);

size_t json_escape_string(const char *src, char *dest, size_t dest_size);

#endif

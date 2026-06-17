#include "unity.h"
#include "../src/json.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_string_simple(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"hello world\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("hello world", out);
}

static void test_string_empty(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_string_escape_newline(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"line1\\nline2\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("line1\nline2", out);
}

static void test_string_escape_tab(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"col1\\tcol2\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("col1\tcol2", out);
}

static void test_string_escape_quotes(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"say \\\"hello\\\"\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("say \"hello\"", out);
}

static void test_string_escape_backslash(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"path\\\\to\\\\file\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("path\\to\\file", out);
}

static void test_string_escape_carriage_return(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"a\\rb\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("a\rb", out);
}

static void test_unicode_simple(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"\\u0041\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("A", out);
}

static void test_unicode_emoji(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"\\uD83D\\uDE00\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING_LEN("\xF0\x9F\x98\x80", out, 4);
}

static void test_unicode_euro(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"\\u20AC\"");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING_LEN("\xE2\x82\xAC", out, 3);
}

static void test_bool_true(void)
{
	json_parser_t p;
	json_parser_init(&p, "true");
	
	bool val;
	TEST_ASSERT_TRUE(json_parse_bool(&p, &val));
	TEST_ASSERT_TRUE(val);
}

static void test_bool_false(void)
{
	json_parser_t p;
	json_parser_init(&p, "false");
	
	bool val;
	TEST_ASSERT_TRUE(json_parse_bool(&p, &val));
	TEST_ASSERT_FALSE(val);
}

static void test_null(void)
{
	json_parser_t p;
	json_parser_init(&p, "null");
	
	TEST_ASSERT_TRUE(json_parse_null(&p));
}

static void test_number_integer(void)
{
	json_parser_t p;
	json_parser_init(&p, "42");
	
	double val;
	TEST_ASSERT_TRUE(json_parse_number(&p, &val));
	TEST_ASSERT_EQUAL_DOUBLE(42.0, val);
}

static void test_number_negative(void)
{
	json_parser_t p;
	json_parser_init(&p, "-17");
	
	double val;
	TEST_ASSERT_TRUE(json_parse_number(&p, &val));
	TEST_ASSERT_EQUAL_DOUBLE(-17.0, val);
}

static void test_number_float(void)
{
	json_parser_t p;
	json_parser_init(&p, "3.14159");
	
	double val;
	TEST_ASSERT_TRUE(json_parse_number(&p, &val));
	TEST_ASSERT_EQUAL_DOUBLE(3.14159, val);
}

static void test_number_scientific(void)
{
	json_parser_t p;
	json_parser_init(&p, "1.5e10");
	
	double val;
	TEST_ASSERT_TRUE(json_parse_number(&p, &val));
	TEST_ASSERT_EQUAL_DOUBLE(1.5e10, val);
}

static void test_number_zero(void)
{
	json_parser_t p;
	json_parser_init(&p, "0");
	
	double val;
	TEST_ASSERT_TRUE(json_parse_number(&p, &val));
	TEST_ASSERT_EQUAL_DOUBLE(0.0, val);
}

static void test_object_empty(void)
{
	json_parser_t p;
	json_parser_init(&p, "{}");
	
	TEST_ASSERT_TRUE(json_object_begin(&p));
	TEST_ASSERT_TRUE(json_peek_char(&p, '}'));
	TEST_ASSERT_TRUE(json_object_end(&p));
}

static void test_object_single_key(void)
{
	json_parser_t p;
	json_parser_init(&p, "{\"name\":\"value\"}");
	
	TEST_ASSERT_TRUE(json_object_begin(&p));
	
	char key[256];
	bool has_more;
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_TRUE(has_more);
	TEST_ASSERT_EQUAL_STRING("name", key);
	
	char val[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, val, sizeof(val)));
	TEST_ASSERT_EQUAL_STRING("value", val);
	
	TEST_ASSERT_TRUE(json_object_end(&p));
}

static void test_object_multiple_keys(void)
{
	json_parser_t p;
	json_parser_init(&p, "{\"a\":1,\"b\":2,\"c\":3}");
	
	TEST_ASSERT_TRUE(json_object_begin(&p));
	
	char key[256];
	bool has_more;
	
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_EQUAL_STRING("a", key);
	double v;
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(1.0, v);
	TEST_ASSERT_TRUE(json_expect_char(&p, ','));
	
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_EQUAL_STRING("b", key);
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(2.0, v);
	TEST_ASSERT_TRUE(json_expect_char(&p, ','));
	
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_EQUAL_STRING("c", key);
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, v);
	
	TEST_ASSERT_TRUE(json_object_end(&p));
}

static void test_array_empty(void)
{
	json_parser_t p;
	json_parser_init(&p, "[]");
	
	TEST_ASSERT_TRUE(json_array_begin(&p));
	TEST_ASSERT_TRUE(json_peek_char(&p, ']'));
	TEST_ASSERT_TRUE(json_array_end(&p));
}

static void test_array_numbers(void)
{
	json_parser_t p;
	json_parser_init(&p, "[1,2,3]");
	
	TEST_ASSERT_TRUE(json_array_begin(&p));
	
	bool has_more;
	double v;
	
	TEST_ASSERT_TRUE(json_array_next(&p, &has_more));
	TEST_ASSERT_TRUE(has_more);
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(1.0, v);
	TEST_ASSERT_TRUE(json_expect_char(&p, ','));
	
	TEST_ASSERT_TRUE(json_array_next(&p, &has_more));
	TEST_ASSERT_TRUE(has_more);
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(2.0, v);
	TEST_ASSERT_TRUE(json_expect_char(&p, ','));
	
	TEST_ASSERT_TRUE(json_array_next(&p, &has_more));
	TEST_ASSERT_TRUE(has_more);
	TEST_ASSERT_TRUE(json_parse_number(&p, &v));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, v);
	
	TEST_ASSERT_TRUE(json_array_end(&p));
}

static void test_nested_object(void)
{
	json_parser_t p;
	json_parser_init(&p, "{\"outer\":{\"inner\":\"value\"}}");
	
	TEST_ASSERT_TRUE(json_object_begin(&p));
	
	char key[256];
	bool has_more;
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_EQUAL_STRING("outer", key);
	
	TEST_ASSERT_TRUE(json_object_begin(&p));
	TEST_ASSERT_TRUE(json_object_next(&p, key, sizeof(key), &has_more));
	TEST_ASSERT_EQUAL_STRING("inner", key);
	
	char val[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, val, sizeof(val)));
	TEST_ASSERT_EQUAL_STRING("value", val);
	
	TEST_ASSERT_TRUE(json_object_end(&p));
	TEST_ASSERT_TRUE(json_object_end(&p));
}

static void test_skip_value_string(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"hello\"");
	
	TEST_ASSERT_TRUE(json_skip_value(&p));
	TEST_ASSERT_TRUE(json_peek_char(&p, '\0') || *p.pos == '\0');
}

static void test_skip_value_object(void)
{
	json_parser_t p;
	json_parser_init(&p, "{\"a\":1,\"b\":{\"c\":2}}");
	
	TEST_ASSERT_TRUE(json_skip_value(&p));
	TEST_ASSERT_TRUE(json_peek_char(&p, '\0') || *p.pos == '\0');
}

static void test_skip_value_array(void)
{
	json_parser_t p;
	json_parser_init(&p, "[1,[2,3],{\"x\":4}]");
	
	TEST_ASSERT_TRUE(json_skip_value(&p));
	TEST_ASSERT_TRUE(json_peek_char(&p, '\0') || *p.pos == '\0');
}

static void test_escape_utility(void)
{
	char escaped[256];
	size_t len = json_escape_string("hello\nworld", escaped, sizeof(escaped));
	
	TEST_ASSERT_TRUE(len > 0);
	TEST_ASSERT_EQUAL_STRING_LEN("\"hello\\nworld\"", escaped, len);
}

static void test_unterminated_string_fails(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"no end quote");
	
	char out[256];
	TEST_ASSERT_FALSE(json_parse_string(&p, out, sizeof(out)));
}

static void test_invalid_escape_fails(void)
{
	json_parser_t p;
	json_parser_init(&p, "\"bad\\xescape\"");
	
	char out[256];
	TEST_ASSERT_FALSE(json_parse_string(&p, out, sizeof(out)));
}

static void test_whitespace_handling(void)
{
	json_parser_t p;
	json_parser_init(&p, "  \n\t  \"value\"  ");
	
	char out[256];
	TEST_ASSERT_TRUE(json_parse_string(&p, out, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("value", out);
}

int main(void)
{
	UnityBegin("test_json.c");
	
	RUN_TEST(test_string_simple);
	RUN_TEST(test_string_empty);
	RUN_TEST(test_string_escape_newline);
	RUN_TEST(test_string_escape_tab);
	RUN_TEST(test_string_escape_quotes);
	RUN_TEST(test_string_escape_backslash);
	RUN_TEST(test_string_escape_carriage_return);
	RUN_TEST(test_unicode_simple);
	RUN_TEST(test_unicode_emoji);
	RUN_TEST(test_unicode_euro);
	
	RUN_TEST(test_bool_true);
	RUN_TEST(test_bool_false);
	RUN_TEST(test_null);
	
	RUN_TEST(test_number_integer);
	RUN_TEST(test_number_negative);
	RUN_TEST(test_number_float);
	RUN_TEST(test_number_scientific);
	RUN_TEST(test_number_zero);
	
	RUN_TEST(test_object_empty);
	RUN_TEST(test_object_single_key);
	RUN_TEST(test_object_multiple_keys);
	RUN_TEST(test_nested_object);
	
	RUN_TEST(test_array_empty);
	RUN_TEST(test_array_numbers);
	
	RUN_TEST(test_skip_value_string);
	RUN_TEST(test_skip_value_object);
	RUN_TEST(test_skip_value_array);
	
	RUN_TEST(test_escape_utility);
	
	RUN_TEST(test_unterminated_string_fails);
	RUN_TEST(test_invalid_escape_fails);
	RUN_TEST(test_whitespace_handling);
	
	return UnityEnd();
}

#ifndef MWS_HTTP_HEADERS_H
#define MWS_HTTP_HEADERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mws_http_header_s {
	char *name;
	char *value;
	struct mws_http_header_s *next;
} mws_http_header_t;

typedef enum {
	MWS_HTTP_HEADER_SET_OK = 0,
	MWS_HTTP_HEADER_SET_INVALID,
	MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED,
	MWS_HTTP_HEADER_SET_NO_MEMORY
} mws_http_header_set_result_t;

bool mws_http_header_name_is_valid(const char *name);

bool mws_http_header_value_is_valid(const char *value);

mws_http_header_set_result_t mws_http_headers_set(
		mws_http_header_t **headers,
		const char *name,
		const char *value);

mws_http_header_set_result_t mws_http_headers_set_limited(
		mws_http_header_t **headers,
		const char *name,
		const char *value,
		uint32_t maximum_count,
		size_t maximum_name_bytes,
		size_t maximum_value_bytes,
		size_t maximum_total_bytes);

uint32_t mws_http_headers_count(const mws_http_header_t *headers);

void mws_http_headers_free(mws_http_header_t *headers);

#endif

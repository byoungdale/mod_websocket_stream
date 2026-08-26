#include "http_headers.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static bool ascii_is_alphanumeric(unsigned char character)
{
	return (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9');
}

static unsigned char ascii_to_lower(unsigned char character)
{
	if (character >= 'A' && character <= 'Z') {
		return (unsigned char) (character + ('a' - 'A'));
	}

	return character;
}

static bool header_names_are_equal(const char *left, const char *right)
{
	while (*left && *right) {
		if (ascii_to_lower((unsigned char) *left) !=
				ascii_to_lower((unsigned char) *right)) {
			return false;
		}
		left++;
		right++;
	}

	return *left == *right;
}

static char *duplicate_string(const char *value)
{
	size_t length = strlen(value) + 1;
	char *copy = malloc(length);

	if (copy) {
		memcpy(copy, value, length);
	}
	return copy;
}

static size_t bounded_string_length(const char *value, size_t maximum)
{
	size_t length = 0;

	if (!value) {
		return maximum + (maximum != SIZE_MAX);
	}
	while (length < maximum && value[length]) {
		length++;
	}
	if (length == maximum && value[length]) {
		return maximum + (maximum != SIZE_MAX);
	}

	return length;
}

static size_t encoded_header_bytes(size_t name_bytes, size_t value_bytes)
{
	return name_bytes + value_bytes + 4;
}

bool mws_http_header_name_is_valid(const char *name)
{
	const unsigned char *character;

	if (!name || !*name) {
		return false;
	}

	for (character = (const unsigned char *) name; *character; character++) {
		if (ascii_is_alphanumeric(*character) ||
				strchr("!#$%&'*+-.^_`|~", *character)) {
			continue;
		}
		return false;
	}

	return true;
}

bool mws_http_header_value_is_valid(const char *value)
{
	return value && !strchr(value, '\r') && !strchr(value, '\n');
}

mws_http_header_set_result_t mws_http_headers_set(
		mws_http_header_t **headers,
		const char *name,
		const char *value)
{
	return mws_http_headers_set_limited(
			headers,
			name,
			value,
			UINT32_MAX,
			SIZE_MAX,
			SIZE_MAX,
			SIZE_MAX);
}

mws_http_header_set_result_t mws_http_headers_set_limited(
		mws_http_header_t **headers,
		const char *name,
		const char *value,
		uint32_t maximum_count,
		size_t maximum_name_bytes,
		size_t maximum_value_bytes,
		size_t maximum_total_bytes)
{
	mws_http_header_t *header;
	mws_http_header_t *matching_header = NULL;
	mws_http_header_t **tail = headers;
	uint32_t count = 0;
	size_t current_total = 0;
	size_t name_bytes;
	size_t value_bytes;
	size_t new_header_bytes;
	char *name_copy;
	char *value_copy;

	if (!headers || !name || !value) {
		return MWS_HTTP_HEADER_SET_INVALID;
	}

	name_bytes = bounded_string_length(name, maximum_name_bytes);
	value_bytes = bounded_string_length(value, maximum_value_bytes);
	if (name_bytes > maximum_name_bytes ||
			value_bytes > maximum_value_bytes) {
		return MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED;
	}
	if (!mws_http_header_name_is_valid(name) ||
			!mws_http_header_value_is_valid(value)) {
		return MWS_HTTP_HEADER_SET_INVALID;
	}
	if (name_bytes > SIZE_MAX - 4 ||
			value_bytes > SIZE_MAX - name_bytes - 4) {
		return MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED;
	}
	new_header_bytes = encoded_header_bytes(name_bytes, value_bytes);

	for (header = *headers; header; header = header->next) {
		size_t old_header_bytes = encoded_header_bytes(
				strlen(header->name),
				strlen(header->value));

		if (current_total > maximum_total_bytes ||
				old_header_bytes > maximum_total_bytes - current_total) {
			return MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED;
		}
		current_total += old_header_bytes;
		count++;
		if (header_names_are_equal(header->name, name)) {
			matching_header = header;
		}
		tail = &header->next;
	}
	if (matching_header) {
		size_t old_header_bytes = encoded_header_bytes(
				strlen(matching_header->name),
				strlen(matching_header->value));
		size_t unchanged_bytes = current_total - old_header_bytes;

		if (new_header_bytes > maximum_total_bytes - unchanged_bytes) {
			return MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED;
		}
	} else if (count >= maximum_count ||
			new_header_bytes > maximum_total_bytes - current_total) {
		return MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED;
	}

	name_copy = duplicate_string(name);
	value_copy = duplicate_string(value);
	if (!name_copy || !value_copy) {
		free(name_copy);
		free(value_copy);
		return MWS_HTTP_HEADER_SET_NO_MEMORY;
	}

	if (matching_header) {
		free(matching_header->name);
		free(matching_header->value);
		matching_header->name = name_copy;
		matching_header->value = value_copy;
		return MWS_HTTP_HEADER_SET_OK;
	}

	header = calloc(1, sizeof(*header));
	if (!header) {
		free(name_copy);
		free(value_copy);
		return MWS_HTTP_HEADER_SET_NO_MEMORY;
	}
	header->name = name_copy;
	header->value = value_copy;
	*tail = header;

	return MWS_HTTP_HEADER_SET_OK;
}

uint32_t mws_http_headers_count(const mws_http_header_t *headers)
{
	uint32_t count = 0;

	for (; headers; headers = headers->next) {
		count++;
	}

	return count;
}

void mws_http_headers_free(mws_http_header_t *headers)
{
	while (headers) {
		mws_http_header_t *next = headers->next;

		free(headers->name);
		free(headers->value);
		free(headers);
		headers = next;
	}
}

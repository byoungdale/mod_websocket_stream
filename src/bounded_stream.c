#include "bounded_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static switch_status_t bounded_raw_write(
		switch_stream_handle_t *stream,
		uint8_t *data,
		switch_size_t data_length)
{
	switch_size_t available;
	switch_size_t copied;

	if (!stream || !stream->data || !stream->data_size) {
		return SWITCH_STATUS_FALSE;
	}
	if (data_length && !data) {
		return SWITCH_STATUS_FALSE;
	}

	available = stream->data_size - 1 - stream->data_len;
	copied = data_length < available ? data_length : available;
	if (copied) {
		memcpy(stream->end, data, copied);
		stream->data_len += copied;
		stream->end = (uint8_t *) stream->data + stream->data_len;
		*((uint8_t *) stream->end) = '\0';
	}
	if (copied != data_length) {
		stream->alloc_chunk = 0;
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t bounded_write(
		switch_stream_handle_t *stream,
		const char *format,
		...)
{
	switch_size_t available;
	int written;
	va_list arguments;

	if (!stream || !stream->data || !stream->data_size || !format) {
		return SWITCH_STATUS_FALSE;
	}

	available = stream->data_size - 1 - stream->data_len;
	va_start(arguments, format);
	written = vsnprintf(
			(char *) stream->end,
			available + 1,
			format,
			arguments);
	va_end(arguments);

	if (written < 0) {
		stream->alloc_chunk = 0;
		return SWITCH_STATUS_FALSE;
	}
	if ((switch_size_t) written > available) {
		stream->data_len = stream->data_size - 1;
		stream->end = (uint8_t *) stream->data + stream->data_len;
		stream->alloc_chunk = 0;
		return SWITCH_STATUS_FALSE;
	}

	stream->data_len += (switch_size_t) written;
	stream->end = (uint8_t *) stream->data + stream->data_len;
	return SWITCH_STATUS_SUCCESS;
}

bool mws_bounded_stream_initialize(
		switch_stream_handle_t *stream,
		size_t maximum_bytes)
{
	return mws_bounded_stream_initialize_with_allocator(
			stream,
			maximum_bytes,
			calloc);
}

bool mws_bounded_stream_initialize_with_allocator(
		switch_stream_handle_t *stream,
		size_t maximum_bytes,
		mws_bounded_stream_calloc_t allocate)
{
	if (!stream || !maximum_bytes || maximum_bytes == SIZE_MAX || !allocate) {
		return false;
	}

	memset(stream, 0, sizeof(*stream));
	stream->data = allocate(maximum_bytes + 1, 1);
	if (!stream->data) {
		return false;
	}
	stream->end = stream->data;
	stream->data_size = maximum_bytes + 1;
	stream->alloc_len = maximum_bytes + 1;
	stream->alloc_chunk = 1;
	stream->write_function = bounded_write;
	stream->raw_write_function = bounded_raw_write;

	return true;
}

bool mws_bounded_stream_overflowed(const switch_stream_handle_t *stream)
{
	return stream && stream->alloc_chunk == 0;
}

void mws_bounded_stream_replace(
		switch_stream_handle_t *stream,
		const char *text)
{
	size_t length;

	if (!stream || !stream->data || !stream->data_size || !text) {
		return;
	}

	length = strlen(text);
	if (length >= stream->data_size) {
		length = stream->data_size - 1;
	}
	memcpy(stream->data, text, length);
	((char *) stream->data)[length] = '\0';
	stream->data_len = length;
	stream->end = (uint8_t *) stream->data + length;
}

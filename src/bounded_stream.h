#ifndef MWS_BOUNDED_STREAM_H
#define MWS_BOUNDED_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

typedef void *(*mws_bounded_stream_calloc_t)(size_t count, size_t size);

bool mws_bounded_stream_initialize(
		switch_stream_handle_t *stream,
		size_t maximum_bytes);

bool mws_bounded_stream_initialize_with_allocator(
		switch_stream_handle_t *stream,
		size_t maximum_bytes,
		mws_bounded_stream_calloc_t allocate);

bool mws_bounded_stream_overflowed(const switch_stream_handle_t *stream);

void mws_bounded_stream_replace(
		switch_stream_handle_t *stream,
		const char *text);

#endif

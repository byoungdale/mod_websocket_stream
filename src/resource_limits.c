#include "resource_limits.h"

#include <assert.h>

size_t mws_bounded_string_length(const char *value, size_t maximum_bytes)
{
	size_t length = 0;

	if (!value) {
		return maximum_bytes + (maximum_bytes != SIZE_MAX);
	}
	while (length < maximum_bytes && value[length]) {
		length++;
	}
	if (length == maximum_bytes && value[length]) {
		return maximum_bytes + (maximum_bytes != SIZE_MAX);
	}

	return length;
}

bool mws_resource_budget_add(
		uint32_t *count,
		size_t *bytes,
		size_t item_bytes,
		uint32_t maximum_count,
		size_t maximum_bytes)
{
	if (!count || !bytes || *count >= maximum_count ||
			*bytes > maximum_bytes || item_bytes > maximum_bytes - *bytes) {
		return false;
	}

	(*count)++;
	*bytes += item_bytes;
	return true;
}

mws_admission_result_t mws_stream_admission_reserve(
		uint32_t *global_count,
		uint32_t *channel_count,
		uint32_t global_limit,
		uint32_t channel_limit)
{
	assert(global_count);
	assert(channel_count);

	if (*global_count >= global_limit) {
		return MWS_ADMISSION_GLOBAL_LIMIT;
	}
	if (*channel_count >= channel_limit) {
		return MWS_ADMISSION_CHANNEL_LIMIT;
	}

	(*global_count)++;
	(*channel_count)++;
	return MWS_ADMISSION_OK;
}

void mws_stream_admission_release(
		uint32_t *global_count,
		uint32_t *channel_count)
{
	assert(global_count && *global_count > 0);
	assert(channel_count && *channel_count > 0);
	(*global_count)--;
	(*channel_count)--;
}

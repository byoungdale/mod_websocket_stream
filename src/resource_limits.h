#ifndef MWS_RESOURCE_LIMITS_H
#define MWS_RESOURCE_LIMITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MWS_DEFAULT_MAXIMUM_STREAMS 256U
#define MWS_HARD_MAXIMUM_STREAMS 4096U
#define MWS_DEFAULT_MAXIMUM_STREAMS_PER_CHANNEL 16U
#define MWS_HARD_MAXIMUM_STREAMS_PER_CHANNEL 64U

#define MWS_MAXIMUM_URL_BYTES 4096U

#define MWS_DEFAULT_MAXIMUM_HTTP_HEADERS 64U
#define MWS_HARD_MAXIMUM_HTTP_HEADERS 256U
#define MWS_MAXIMUM_HTTP_HEADER_NAME_BYTES 256U
#define MWS_MAXIMUM_HTTP_HEADER_VALUE_BYTES 4096U
#define MWS_DEFAULT_MAXIMUM_HTTP_HEADER_BYTES 32768U
#define MWS_HARD_MAXIMUM_HTTP_HEADER_BYTES 262144U

#define MWS_DEFAULT_MAXIMUM_START_VARIABLES 256U
#define MWS_HARD_MAXIMUM_START_VARIABLES 2048U
#define MWS_MAXIMUM_START_VARIABLE_NAME_BYTES 256U
#define MWS_MAXIMUM_START_VARIABLE_VALUE_BYTES 4096U
#define MWS_DEFAULT_MAXIMUM_START_VARIABLE_BYTES 32768U
#define MWS_HARD_MAXIMUM_START_VARIABLE_BYTES 262144U
#define MWS_DEFAULT_MAXIMUM_START_MESSAGE_BYTES 65536U
#define MWS_HARD_MAXIMUM_START_MESSAGE_BYTES 1048576U

#define MWS_DEFAULT_MAXIMUM_API_RESPONSE_BYTES 65536U
#define MWS_MINIMUM_API_RESPONSE_BYTES 256U
#define MWS_HARD_MAXIMUM_API_RESPONSE_BYTES 1048576U

typedef enum {
	MWS_ADMISSION_OK = 0,
	MWS_ADMISSION_GLOBAL_LIMIT,
	MWS_ADMISSION_CHANNEL_LIMIT,
	MWS_ADMISSION_NO_MEMORY
} mws_admission_result_t;

size_t mws_bounded_string_length(const char *value, size_t maximum_bytes);

bool mws_resource_budget_add(
		uint32_t *count,
		size_t *bytes,
		size_t item_bytes,
		uint32_t maximum_count,
		size_t maximum_bytes);

mws_admission_result_t mws_stream_admission_reserve(
		uint32_t *global_count,
		uint32_t *channel_count,
		uint32_t global_limit,
		uint32_t channel_limit);

void mws_stream_admission_release(
		uint32_t *global_count,
		uint32_t *channel_count);

#endif

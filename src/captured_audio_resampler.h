#ifndef MWS_CAPTURED_AUDIO_RESAMPLER_H
#define MWS_CAPTURED_AUDIO_RESAMPLER_H

#include <switch.h>

#include <stdatomic.h>
#include <stdint.h>

#include "captured_audio_buffer.h"

typedef enum {
	MWS_CAPTURED_AUDIO_RESAMPLER_ERROR = -1,
	MWS_CAPTURED_AUDIO_RESAMPLER_EMPTY = 0,
	MWS_CAPTURED_AUDIO_RESAMPLER_OUTPUT = 1
} mws_captured_audio_resampler_result_t;

typedef struct {
	uint8_t *source_buffer;
	switch_size_t source_capacity_bytes;
	uint8_t *target_buffer;
	switch_size_t target_capacity_bytes;
	switch_size_t target_offset_bytes;
	switch_size_t target_length_bytes;
	switch_audio_resampler_t *freeswitch_resampler;
	uint32_t target_sample_rate;
	uint32_t minimum_source_sample_rate;
	uint32_t maximum_source_sample_rate;
	uint32_t maximum_source_channels;
	uint64_t source_format_generation;
	uint32_t source_sample_rate;
	uint32_t source_channels;
	const char *session_uuid;
	_Atomic uint64_t source_format_change_count;
	_Atomic uint64_t source_consumed_byte_count;
	_Atomic uint64_t target_produced_byte_count;
	_Atomic uint64_t resampler_create_count;
	_Atomic uint64_t resampler_error_count;
	_Atomic uint64_t source_processing_failed_byte_count;
	_Atomic uint64_t pending_target_dropped_byte_count;
	_Atomic uint64_t published_source_format_sequence;
	_Atomic uint64_t published_source_format_generation;
	_Atomic uint32_t published_source_sample_rate;
	_Atomic uint32_t published_source_channels;
} mws_captured_audio_resampler_t;

typedef struct {
	uint64_t source_format_generation;
	uint64_t source_format_changes;
	uint32_t source_sample_rate;
	uint32_t source_channels;
	uint64_t source_consumed_bytes;
	uint64_t target_produced_bytes;
	uint64_t resampler_creates;
	uint64_t resampler_errors;
	uint64_t source_processing_failed_bytes;
	uint64_t pending_target_dropped_bytes;
} mws_captured_audio_resampler_statistics_t;

switch_status_t mws_captured_audio_resampler_initialize(
		mws_captured_audio_resampler_t *resampler,
		uint8_t *source_buffer,
		switch_size_t source_capacity_bytes,
		uint8_t *target_buffer,
		switch_size_t target_capacity_bytes,
		uint32_t target_sample_rate,
		uint32_t minimum_source_sample_rate,
		uint32_t maximum_source_sample_rate,
		uint32_t maximum_source_channels,
		const char *session_uuid);

mws_captured_audio_resampler_result_t mws_captured_audio_resampler_read(
		mws_captured_audio_resampler_t *resampler,
		mws_captured_audio_buffer_t *captured_audio,
		uint8_t *output,
		switch_size_t output_capacity_bytes,
		switch_size_t *output_length_bytes);

switch_bool_t mws_captured_audio_resampler_has_pending_output(
		const mws_captured_audio_resampler_t *resampler);

void mws_captured_audio_resampler_reset(
		mws_captured_audio_resampler_t *resampler);

void mws_captured_audio_resampler_snapshot(
		const mws_captured_audio_resampler_t *resampler,
		mws_captured_audio_resampler_statistics_t *statistics);

#endif

#ifndef MWS_PLAYBACK_AUDIO_RESAMPLER_H
#define MWS_PLAYBACK_AUDIO_RESAMPLER_H

#include <speex/speex_resampler.h>
#include <switch.h>

#include <stdatomic.h>
#include <stdint.h>

#include "playback_rate_controller.h"
#include "source_audio_buffer.h"

typedef enum {
	MWS_PLAYBACK_AUDIO_RESAMPLER_ERROR = -1,
	MWS_PLAYBACK_AUDIO_RESAMPLER_EMPTY = 0,
	MWS_PLAYBACK_AUDIO_RESAMPLER_OUTPUT = 1
} mws_playback_audio_resampler_result_t;

typedef struct {
	const uint8_t *data;
	uint32_t length_bytes;
	uint32_t consumed_source_bytes;
} mws_playback_audio_resampler_output_t;

typedef struct {
	uint64_t playback_format_generation;
	uint32_t playback_sample_rate;
	uint64_t target_queue_us;
	uint64_t filtered_queue_us;
	int32_t rate_adjustment_ppm;
	uint32_t maximum_rate_adjustment_ppm;
	uint64_t rate_update_count;
	uint64_t rate_limit_count;
	uint64_t resampler_create_count;
	uint64_t resampler_error_count;
} mws_playback_audio_resampler_statistics_t;

typedef struct {
	uint8_t *source_buffer;
	uint32_t source_capacity_bytes;
	uint8_t *output_buffer;
	uint32_t output_capacity_bytes;
	SpeexResamplerState *speex_resampler;
	uint32_t source_sample_rate;
	uint32_t minimum_playback_sample_rate;
	uint32_t maximum_playback_sample_rate;
	uint64_t playback_format_generation;
	uint32_t playback_sample_rate;
	switch_time_t next_rate_update_at;
	uint64_t rate_update_interval_us;
	mws_playback_rate_controller_t rate_controller;
	int32_t applied_rate_adjustment_ppm;
	const char *session_uuid;
	_Atomic uint64_t published_playback_format_generation;
	_Atomic uint32_t published_playback_sample_rate;
	_Atomic int32_t published_rate_adjustment_ppm;
	_Atomic uint32_t published_maximum_rate_adjustment_ppm;
	_Atomic uint64_t published_filtered_queue_us;
	_Atomic uint64_t published_rate_update_count;
	_Atomic uint64_t published_rate_limit_count;
	_Atomic uint64_t resampler_create_count;
	_Atomic uint64_t resampler_error_count;
} mws_playback_audio_resampler_t;

switch_status_t mws_playback_audio_resampler_initialize(
		mws_playback_audio_resampler_t *resampler,
		uint8_t *source_buffer,
		uint32_t source_capacity_bytes,
		uint8_t *output_buffer,
		uint32_t output_capacity_bytes,
		uint32_t source_sample_rate,
		uint32_t minimum_playback_sample_rate,
		uint32_t maximum_playback_sample_rate,
		uint64_t target_queue_us,
		uint64_t rate_update_interval_us,
		const char *session_uuid);

switch_status_t mws_playback_audio_resampler_update(
		mws_playback_audio_resampler_t *resampler,
		uint64_t playback_format_generation,
		uint32_t playback_sample_rate,
		switch_bool_t playback_active,
		uint64_t source_queued_sample_count,
		uint64_t playback_queued_sample_count,
		switch_time_t now_us);

mws_playback_audio_resampler_result_t mws_playback_audio_resampler_convert(
		mws_playback_audio_resampler_t *resampler,
		const mws_source_audio_buffer_t *source_audio,
		uint32_t maximum_output_bytes,
		mws_playback_audio_resampler_output_t *output);

void mws_playback_audio_resampler_reset(
		mws_playback_audio_resampler_t *resampler);

void mws_playback_audio_resampler_snapshot(
		const mws_playback_audio_resampler_t *resampler,
		mws_playback_audio_resampler_statistics_t *statistics);

#endif

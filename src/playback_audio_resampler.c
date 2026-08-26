#include "playback_audio_resampler.h"

#include <string.h>

#define MWS_PLAYBACK_RATE_RESOLUTION_PPM 100
#define MWS_PLAYBACK_RATE_RATIO_SCALE 10000

static void publish_rate_statistics(
		mws_playback_audio_resampler_t *resampler)
{
	mws_playback_rate_controller_t *controller =
			&resampler->rate_controller;

	atomic_store_explicit(
			&resampler->published_rate_adjustment_ppm,
			controller->adjustment_ppm,
			memory_order_release);
	atomic_store_explicit(
			&resampler->published_maximum_rate_adjustment_ppm,
			controller->maximum_adjustment_ppm,
			memory_order_release);
	atomic_store_explicit(
			&resampler->published_filtered_queue_us,
			(uint64_t) controller->filtered_queue_us,
			memory_order_release);
	atomic_store_explicit(
			&resampler->published_rate_update_count,
			controller->update_count,
			memory_order_release);
	atomic_store_explicit(
			&resampler->published_rate_limit_count,
			controller->limit_count,
			memory_order_release);
}

static void publish_playback_format(
		mws_playback_audio_resampler_t *resampler)
{
	atomic_store_explicit(
			&resampler->published_playback_format_generation,
			resampler->playback_format_generation,
			memory_order_release);
	atomic_store_explicit(
			&resampler->published_playback_sample_rate,
			resampler->playback_sample_rate,
			memory_order_release);
}

static switch_status_t configure_playback_format(
		mws_playback_audio_resampler_t *resampler,
		uint64_t playback_format_generation,
		uint32_t playback_sample_rate)
{
	int error = RESAMPLER_ERR_SUCCESS;

	if (resampler->playback_format_generation ==
			playback_format_generation &&
			resampler->playback_sample_rate == playback_sample_rate &&
			resampler->speex_resampler) {
		return SWITCH_STATUS_SUCCESS;
	}
	if (!playback_format_generation ||
			playback_sample_rate < resampler->minimum_playback_sample_rate ||
			playback_sample_rate > resampler->maximum_playback_sample_rate) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		return SWITCH_STATUS_FALSE;
	}

	if (resampler->speex_resampler) {
		speex_resampler_destroy(resampler->speex_resampler);
		resampler->speex_resampler = NULL;
	}

	resampler->playback_format_generation = playback_format_generation;
	resampler->playback_sample_rate = playback_sample_rate;
	resampler->next_rate_update_at = 0;
	mws_playback_rate_controller_reset(&resampler->rate_controller);
	resampler->applied_rate_adjustment_ppm = 0;
	publish_playback_format(resampler);
	publish_rate_statistics(resampler);

	resampler->speex_resampler = speex_resampler_init(
			1,
			resampler->source_sample_rate,
			playback_sample_rate,
			SPEEX_RESAMPLER_QUALITY_DEFAULT,
			&error);
	if (!resampler->speex_resampler || error != RESAMPLER_ERR_SUCCESS) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		if (resampler->speex_resampler) {
			speex_resampler_destroy(resampler->speex_resampler);
			resampler->speex_resampler = NULL;
		}
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"Could not create WebSocket playback resampler: "
				"uuid=%s source_rate_hz=%u playback_rate_hz=%u "
				"error=%s\n",
				resampler->session_uuid ? resampler->session_uuid : "unknown",
				resampler->source_sample_rate,
				playback_sample_rate,
				speex_resampler_strerror(error));
		return SWITCH_STATUS_FALSE;
	}

	atomic_fetch_add_explicit(
			&resampler->resampler_create_count,
			1,
			memory_order_relaxed);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t set_rate_adjustment(
		mws_playback_audio_resampler_t *resampler,
		int32_t adjustment_ppm)
{
	int32_t adjustment_units;
	spx_uint32_t ratio_numerator;
	spx_uint32_t ratio_denominator;
	int error;

	if (!resampler->speex_resampler) {
		return SWITCH_STATUS_FALSE;
	}
	if (adjustment_ppm >= 0) {
		adjustment_units =
				(adjustment_ppm + MWS_PLAYBACK_RATE_RESOLUTION_PPM / 2) /
				MWS_PLAYBACK_RATE_RESOLUTION_PPM;
	} else {
		adjustment_units = -(
				(-adjustment_ppm + MWS_PLAYBACK_RATE_RESOLUTION_PPM / 2) /
				MWS_PLAYBACK_RATE_RESOLUTION_PPM);
	}
	adjustment_ppm = adjustment_units * MWS_PLAYBACK_RATE_RESOLUTION_PPM;
	if (adjustment_ppm == resampler->applied_rate_adjustment_ppm) {
		return SWITCH_STATUS_SUCCESS;
	}

	/* A larger input/output ratio consumes queued source audio faster. */
	ratio_numerator = resampler->source_sample_rate *
			(MWS_PLAYBACK_RATE_RATIO_SCALE + adjustment_units);
	ratio_denominator = resampler->playback_sample_rate *
			MWS_PLAYBACK_RATE_RATIO_SCALE;
	error = speex_resampler_set_rate_frac(
			resampler->speex_resampler,
			ratio_numerator,
			ratio_denominator,
			resampler->source_sample_rate,
			resampler->playback_sample_rate);
	if (error != RESAMPLER_ERR_SUCCESS) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"Could not adjust WebSocket playback rate: "
				"uuid=%s error=%s\n",
				resampler->session_uuid ? resampler->session_uuid : "unknown",
				speex_resampler_strerror(error));
		return SWITCH_STATUS_FALSE;
	}

	resampler->applied_rate_adjustment_ppm = adjustment_ppm;
	return SWITCH_STATUS_SUCCESS;
}

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
		const char *session_uuid)
{
	if (!resampler || !source_buffer || !source_capacity_bytes ||
			!output_buffer || !output_capacity_bytes ||
			!minimum_playback_sample_rate ||
			maximum_playback_sample_rate < minimum_playback_sample_rate ||
			source_sample_rate < minimum_playback_sample_rate ||
			source_sample_rate > maximum_playback_sample_rate ||
			!target_queue_us || !rate_update_interval_us ||
			source_capacity_bytes % sizeof(int16_t) ||
			output_capacity_bytes % sizeof(int16_t)) {
		return SWITCH_STATUS_FALSE;
	}

	memset(resampler, 0, sizeof(*resampler));
	resampler->source_buffer = source_buffer;
	resampler->source_capacity_bytes = source_capacity_bytes;
	resampler->output_buffer = output_buffer;
	resampler->output_capacity_bytes = output_capacity_bytes;
	resampler->source_sample_rate = source_sample_rate;
	resampler->minimum_playback_sample_rate = minimum_playback_sample_rate;
	resampler->maximum_playback_sample_rate = maximum_playback_sample_rate;
	resampler->rate_update_interval_us = rate_update_interval_us;
	resampler->session_uuid = session_uuid;
	mws_playback_rate_controller_init(
			&resampler->rate_controller,
			(int64_t) target_queue_us);
	atomic_init(&resampler->published_playback_format_generation, 0);
	atomic_init(&resampler->published_playback_sample_rate, 0);
	atomic_init(&resampler->published_rate_adjustment_ppm, 0);
	atomic_init(&resampler->published_maximum_rate_adjustment_ppm, 0);
	atomic_init(&resampler->published_filtered_queue_us, target_queue_us);
	atomic_init(&resampler->published_rate_update_count, 0);
	atomic_init(&resampler->published_rate_limit_count, 0);
	atomic_init(&resampler->resampler_create_count, 0);
	atomic_init(&resampler->resampler_error_count, 0);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t mws_playback_audio_resampler_update(
		mws_playback_audio_resampler_t *resampler,
		uint64_t playback_format_generation,
		uint32_t playback_sample_rate,
		switch_bool_t playback_active,
		uint64_t source_queued_sample_count,
		uint64_t playback_queued_sample_count,
		switch_time_t now_us)
{
	uint64_t queued_audio_us;
	int32_t next_adjustment_ppm;
	switch_status_t status;

	if (!resampler || configure_playback_format(
			resampler,
			playback_format_generation,
			playback_sample_rate) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!playback_active) {
		resampler->next_rate_update_at = 0;
		mws_playback_rate_controller_reset(&resampler->rate_controller);
		status = set_rate_adjustment(resampler, 0);
		publish_rate_statistics(resampler);
		return status;
	}

	queued_audio_us = mws_playback_queued_duration_us(
			source_queued_sample_count,
			resampler->source_sample_rate,
			playback_queued_sample_count,
			playback_sample_rate);
	mws_playback_rate_controller_observe_queue(
			&resampler->rate_controller,
			queued_audio_us);

	if (!resampler->next_rate_update_at) {
		resampler->next_rate_update_at = now_us +
				(switch_time_t) resampler->rate_update_interval_us;
		return SWITCH_STATUS_SUCCESS;
	}
	if (now_us < resampler->next_rate_update_at) {
		return SWITCH_STATUS_SUCCESS;
	}
	resampler->next_rate_update_at = now_us +
			(switch_time_t) resampler->rate_update_interval_us;
	next_adjustment_ppm =
			mws_playback_rate_controller_update_from_observations(
					&resampler->rate_controller);
	status = set_rate_adjustment(resampler, next_adjustment_ppm);
	publish_rate_statistics(resampler);
	return status;
}

mws_playback_audio_resampler_result_t mws_playback_audio_resampler_convert(
		mws_playback_audio_resampler_t *resampler,
		const mws_source_audio_buffer_t *source_audio,
		uint32_t maximum_output_bytes,
		mws_playback_audio_resampler_output_t *output)
{
	uint32_t source_length_bytes;
	spx_uint32_t input_sample_count;
	spx_uint32_t output_sample_count;
	int error;

	if (output) {
		memset(output, 0, sizeof(*output));
	}
	if (!resampler || !source_audio || !output ||
			!resampler->speex_resampler) {
		return MWS_PLAYBACK_AUDIO_RESAMPLER_ERROR;
	}

	if (maximum_output_bytes > resampler->output_capacity_bytes) {
		maximum_output_bytes = resampler->output_capacity_bytes;
	}
	maximum_output_bytes -= maximum_output_bytes % sizeof(int16_t);
	if (!maximum_output_bytes) {
		return MWS_PLAYBACK_AUDIO_RESAMPLER_EMPTY;
	}

	source_length_bytes = mws_source_audio_buffer_copy(
			source_audio,
			resampler->source_buffer,
			resampler->source_capacity_bytes);
	if (!source_length_bytes) {
		return MWS_PLAYBACK_AUDIO_RESAMPLER_EMPTY;
	}

	input_sample_count = source_length_bytes / sizeof(int16_t);
	output_sample_count = maximum_output_bytes / sizeof(int16_t);
	error = speex_resampler_process_int(
			resampler->speex_resampler,
			0,
			(const spx_int16_t *) resampler->source_buffer,
			&input_sample_count,
			(spx_int16_t *) resampler->output_buffer,
			&output_sample_count);
	if (error != RESAMPLER_ERR_SUCCESS) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"Could not resample WebSocket playback audio: "
				"uuid=%s error=%s\n",
				resampler->session_uuid ? resampler->session_uuid : "unknown",
				speex_resampler_strerror(error));
		return MWS_PLAYBACK_AUDIO_RESAMPLER_ERROR;
	}

	output->data = resampler->output_buffer;
	output->length_bytes = output_sample_count * sizeof(int16_t);
	output->consumed_source_bytes = input_sample_count * sizeof(int16_t);
	return output->length_bytes || output->consumed_source_bytes ?
			MWS_PLAYBACK_AUDIO_RESAMPLER_OUTPUT :
			MWS_PLAYBACK_AUDIO_RESAMPLER_EMPTY;
}

void mws_playback_audio_resampler_reset(
		mws_playback_audio_resampler_t *resampler)
{
	if (!resampler) {
		return;
	}
	if (resampler->speex_resampler) {
		speex_resampler_destroy(resampler->speex_resampler);
		resampler->speex_resampler = NULL;
	}
	resampler->playback_format_generation = 0;
	resampler->playback_sample_rate = 0;
	resampler->next_rate_update_at = 0;
	resampler->applied_rate_adjustment_ppm = 0;
	mws_playback_rate_controller_reset(&resampler->rate_controller);
	publish_playback_format(resampler);
	publish_rate_statistics(resampler);
}

void mws_playback_audio_resampler_snapshot(
		const mws_playback_audio_resampler_t *resampler,
		mws_playback_audio_resampler_statistics_t *statistics)
{
	if (!statistics) {
		return;
	}
	memset(statistics, 0, sizeof(*statistics));
	if (!resampler) {
		return;
	}

	statistics->playback_format_generation = atomic_load_explicit(
			&resampler->published_playback_format_generation,
			memory_order_acquire);
	statistics->playback_sample_rate = atomic_load_explicit(
			&resampler->published_playback_sample_rate,
			memory_order_acquire);
	statistics->target_queue_us =
			(uint64_t) resampler->rate_controller.target_queue_us;
	statistics->filtered_queue_us = atomic_load_explicit(
			&resampler->published_filtered_queue_us,
			memory_order_acquire);
	statistics->rate_adjustment_ppm = atomic_load_explicit(
			&resampler->published_rate_adjustment_ppm,
			memory_order_acquire);
	statistics->maximum_rate_adjustment_ppm = atomic_load_explicit(
			&resampler->published_maximum_rate_adjustment_ppm,
			memory_order_acquire);
	statistics->rate_update_count = atomic_load_explicit(
			&resampler->published_rate_update_count,
			memory_order_acquire);
	statistics->rate_limit_count = atomic_load_explicit(
			&resampler->published_rate_limit_count,
			memory_order_acquire);
	statistics->resampler_create_count = atomic_load_explicit(
			&resampler->resampler_create_count,
			memory_order_relaxed);
	statistics->resampler_error_count = atomic_load_explicit(
			&resampler->resampler_error_count,
			memory_order_relaxed);
}

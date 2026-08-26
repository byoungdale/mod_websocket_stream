#include "captured_audio_resampler.h"

#include <string.h>

static void publish_source_format(
		mws_captured_audio_resampler_t *resampler)
{
	atomic_fetch_add_explicit(
			&resampler->published_source_format_sequence,
			1,
			memory_order_acq_rel);
	atomic_store_explicit(
			&resampler->published_source_sample_rate,
			resampler->source_sample_rate,
			memory_order_relaxed);
	atomic_store_explicit(
			&resampler->published_source_channels,
			resampler->source_channels,
			memory_order_relaxed);
	atomic_store_explicit(
			&resampler->published_source_format_generation,
			resampler->source_format_generation,
			memory_order_relaxed);
	atomic_fetch_add_explicit(
			&resampler->published_source_format_sequence,
			1,
			memory_order_release);
}

static switch_status_t configure_source_format(
		mws_captured_audio_resampler_t *resampler,
		uint64_t format_generation,
		uint32_t source_sample_rate,
		uint32_t source_channels)
{
	switch_bool_t format_changed;
	switch_size_t pcm_frame_size_bytes;
	uint32_t input_sample_frame_capacity;

	format_changed = resampler->source_format_generation != format_generation ||
			resampler->source_sample_rate != source_sample_rate ||
			resampler->source_channels != source_channels;
	if (!format_changed &&
			(source_sample_rate == resampler->target_sample_rate ||
			 resampler->freeswitch_resampler)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (source_sample_rate < resampler->minimum_source_sample_rate ||
			source_sample_rate > resampler->maximum_source_sample_rate ||
			!source_channels ||
			source_channels > resampler->maximum_source_channels) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		return SWITCH_STATUS_FALSE;
	}

	if (resampler->target_length_bytes != resampler->target_offset_bytes) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		return SWITCH_STATUS_FALSE;
	}

	if (format_changed && resampler->source_format_generation) {
		atomic_fetch_add_explicit(
				&resampler->source_format_change_count,
				1,
				memory_order_relaxed);
	}

	if (resampler->freeswitch_resampler) {
		switch_resample_destroy(&resampler->freeswitch_resampler);
	}

	resampler->source_format_generation = format_generation;
	resampler->source_sample_rate = source_sample_rate;
	resampler->source_channels = source_channels;
	publish_source_format(resampler);

	if (source_sample_rate == resampler->target_sample_rate) {
		return SWITCH_STATUS_SUCCESS;
	}

	pcm_frame_size_bytes = source_channels * sizeof(int16_t);
	input_sample_frame_capacity = (uint32_t) (
			resampler->source_capacity_bytes / pcm_frame_size_bytes);

	if (!input_sample_frame_capacity ||
			switch_resample_create(
					&resampler->freeswitch_resampler,
					source_sample_rate,
					resampler->target_sample_rate,
					input_sample_frame_capacity,
					SWITCH_RESAMPLE_QUALITY,
					source_channels) != SWITCH_STATUS_SUCCESS) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		if (resampler->freeswitch_resampler) {
			switch_resample_destroy(&resampler->freeswitch_resampler);
		}
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"Could not create PCM resampler: uuid=%s "
				"source_rate_hz=%u target_rate_hz=%u channels=%u\n",
				resampler->session_uuid ? resampler->session_uuid : "unknown",
				source_sample_rate,
				resampler->target_sample_rate,
				source_channels);
		return SWITCH_STATUS_FALSE;
	}

	atomic_fetch_add_explicit(
			&resampler->resampler_create_count,
			1,
			memory_order_relaxed);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t stage_target_audio(
		mws_captured_audio_resampler_t *resampler,
		mws_captured_audio_buffer_t *captured_audio,
		switch_bool_t *target_audio_available,
		switch_bool_t *source_audio_consumed)
{
	uint64_t source_format_generation = 0;
	uint32_t source_sample_rate = 0;
	uint32_t source_channels = 0;
	uint32_t input_sample_frames;
	uint32_t output_sample_frames;
	switch_size_t source_length_bytes;
	switch_size_t target_length_bytes;
	switch_size_t pcm_frame_size_bytes;

	*target_audio_available = SWITCH_FALSE;
	*source_audio_consumed = SWITCH_FALSE;
	source_length_bytes = mws_captured_audio_buffer_read(
			captured_audio,
			resampler->source_buffer,
			resampler->source_capacity_bytes,
			&source_format_generation,
			&source_sample_rate,
			&source_channels);

	if (!source_length_bytes) {
		return SWITCH_STATUS_SUCCESS;
	}
	*source_audio_consumed = SWITCH_TRUE;
	atomic_fetch_add_explicit(
			&resampler->source_consumed_byte_count,
			source_length_bytes,
			memory_order_relaxed);

	if (configure_source_format(
			resampler,
			source_format_generation,
			source_sample_rate,
			source_channels) != SWITCH_STATUS_SUCCESS) {
		goto processing_failed;
	}

	pcm_frame_size_bytes = source_channels * sizeof(int16_t);
	if (!pcm_frame_size_bytes ||
			source_length_bytes % pcm_frame_size_bytes) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		goto processing_failed;
	}

	resampler->target_offset_bytes = 0;

	if (!resampler->freeswitch_resampler) {
		if (source_length_bytes > resampler->target_capacity_bytes) {
			atomic_fetch_add_explicit(
					&resampler->resampler_error_count,
					1,
					memory_order_relaxed);
			goto processing_failed;
		}

		memcpy(
				resampler->target_buffer,
				resampler->source_buffer,
				source_length_bytes);
		target_length_bytes = source_length_bytes;
	} else {
		input_sample_frames = (uint32_t) (
				source_length_bytes / pcm_frame_size_bytes);
		output_sample_frames = switch_resample_process(
				resampler->freeswitch_resampler,
				(int16_t *) resampler->source_buffer,
				input_sample_frames);

		if (output_sample_frames >
				resampler->target_capacity_bytes / pcm_frame_size_bytes) {
			atomic_fetch_add_explicit(
					&resampler->resampler_error_count,
					1,
					memory_order_relaxed);
			goto processing_failed;
		}

		target_length_bytes = (switch_size_t) output_sample_frames *
				pcm_frame_size_bytes;
		if (target_length_bytes) {
			memcpy(
					resampler->target_buffer,
					resampler->freeswitch_resampler->to,
					target_length_bytes);
		}
	}

	resampler->target_length_bytes = target_length_bytes;
	atomic_fetch_add_explicit(
			&resampler->target_produced_byte_count,
			target_length_bytes,
			memory_order_relaxed);
	*target_audio_available = target_length_bytes ?
			SWITCH_TRUE : SWITCH_FALSE;

	return SWITCH_STATUS_SUCCESS;

processing_failed:
	atomic_fetch_add_explicit(
			&resampler->source_processing_failed_byte_count,
			source_length_bytes,
			memory_order_relaxed);
	return SWITCH_STATUS_FALSE;
}

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
		const char *session_uuid)
{
	if (!resampler || !source_buffer || !source_capacity_bytes ||
			!target_buffer || !target_capacity_bytes ||
			!target_sample_rate || !minimum_source_sample_rate ||
			maximum_source_sample_rate < minimum_source_sample_rate ||
			target_sample_rate < minimum_source_sample_rate ||
			target_sample_rate > maximum_source_sample_rate ||
			!maximum_source_channels ||
			source_capacity_bytes % sizeof(int16_t) ||
			target_capacity_bytes % sizeof(int16_t)) {
		return SWITCH_STATUS_FALSE;
	}

	memset(resampler, 0, sizeof(*resampler));
	resampler->source_buffer = source_buffer;
	resampler->source_capacity_bytes = source_capacity_bytes;
	resampler->target_buffer = target_buffer;
	resampler->target_capacity_bytes = target_capacity_bytes;
	resampler->target_sample_rate = target_sample_rate;
	resampler->minimum_source_sample_rate = minimum_source_sample_rate;
	resampler->maximum_source_sample_rate = maximum_source_sample_rate;
	resampler->maximum_source_channels = maximum_source_channels;
	resampler->session_uuid = session_uuid;
	atomic_init(&resampler->source_format_change_count, 0);
	atomic_init(&resampler->source_consumed_byte_count, 0);
	atomic_init(&resampler->target_produced_byte_count, 0);
	atomic_init(&resampler->resampler_create_count, 0);
	atomic_init(&resampler->resampler_error_count, 0);
	atomic_init(&resampler->source_processing_failed_byte_count, 0);
	atomic_init(&resampler->pending_target_dropped_byte_count, 0);
	atomic_init(&resampler->published_source_format_sequence, 0);
	atomic_init(&resampler->published_source_format_generation, 0);
	atomic_init(&resampler->published_source_sample_rate, 0);
	atomic_init(&resampler->published_source_channels, 0);

	return SWITCH_STATUS_SUCCESS;
}

mws_captured_audio_resampler_result_t mws_captured_audio_resampler_read(
		mws_captured_audio_resampler_t *resampler,
		mws_captured_audio_buffer_t *captured_audio,
		uint8_t *output,
		switch_size_t output_capacity_bytes,
		switch_size_t *output_length_bytes)
{
	switch_bool_t source_audio_consumed;
	switch_bool_t target_audio_available;
	switch_size_t pending_target_bytes;
	switch_size_t pcm_frame_size_bytes;
	switch_size_t copy_length_bytes;

	if (output_length_bytes) {
		*output_length_bytes = 0;
	}
	if (!resampler || !captured_audio || !output ||
			!output_capacity_bytes || !output_length_bytes) {
		return MWS_CAPTURED_AUDIO_RESAMPLER_ERROR;
	}

	while (resampler->target_offset_bytes == resampler->target_length_bytes) {
		resampler->target_offset_bytes = 0;
		resampler->target_length_bytes = 0;

		if (stage_target_audio(
				resampler,
				captured_audio,
				&target_audio_available,
				&source_audio_consumed) != SWITCH_STATUS_SUCCESS) {
			return MWS_CAPTURED_AUDIO_RESAMPLER_ERROR;
		}

		if (!target_audio_available) {
			if (source_audio_consumed) {
				continue;
			}
			return MWS_CAPTURED_AUDIO_RESAMPLER_EMPTY;
		}
	}

	pcm_frame_size_bytes = resampler->source_channels * sizeof(int16_t);
	if (!pcm_frame_size_bytes ||
			output_capacity_bytes < pcm_frame_size_bytes) {
		atomic_fetch_add_explicit(
				&resampler->resampler_error_count,
				1,
				memory_order_relaxed);
		return MWS_CAPTURED_AUDIO_RESAMPLER_ERROR;
	}

	pending_target_bytes = resampler->target_length_bytes -
			resampler->target_offset_bytes;
	copy_length_bytes = pending_target_bytes < output_capacity_bytes ?
			pending_target_bytes : output_capacity_bytes;
	copy_length_bytes -= copy_length_bytes % pcm_frame_size_bytes;

	memcpy(
			output,
			resampler->target_buffer + resampler->target_offset_bytes,
			copy_length_bytes);
	resampler->target_offset_bytes += copy_length_bytes;
	if (resampler->target_offset_bytes == resampler->target_length_bytes) {
		resampler->target_offset_bytes = 0;
		resampler->target_length_bytes = 0;
	}

	*output_length_bytes = copy_length_bytes;
	return MWS_CAPTURED_AUDIO_RESAMPLER_OUTPUT;
}

switch_bool_t mws_captured_audio_resampler_has_pending_output(
		const mws_captured_audio_resampler_t *resampler)
{
	return resampler && resampler->target_length_bytes >
			resampler->target_offset_bytes ? SWITCH_TRUE : SWITCH_FALSE;
}

void mws_captured_audio_resampler_reset(
		mws_captured_audio_resampler_t *resampler)
{
	switch_size_t pending_bytes;

	if (!resampler) {
		return;
	}

	pending_bytes = resampler->target_length_bytes >
			resampler->target_offset_bytes ?
			resampler->target_length_bytes -
				resampler->target_offset_bytes : 0;
	atomic_fetch_add_explicit(
			&resampler->pending_target_dropped_byte_count,
			pending_bytes,
			memory_order_relaxed);
	resampler->target_offset_bytes = 0;
	resampler->target_length_bytes = 0;

	if (resampler->freeswitch_resampler) {
		switch_resample_destroy(&resampler->freeswitch_resampler);
	}
}

void mws_captured_audio_resampler_snapshot(
		const mws_captured_audio_resampler_t *resampler,
		mws_captured_audio_resampler_statistics_t *statistics)
{
	uint64_t sequence_before;
	uint64_t sequence_after;

	if (!resampler || !statistics) {
		return;
	}

	memset(statistics, 0, sizeof(*statistics));
	for (;;) {
		sequence_before = atomic_load_explicit(
				&resampler->published_source_format_sequence,
				memory_order_acquire);
		if (sequence_before & 1) {
			continue;
		}
		statistics->source_format_generation = atomic_load_explicit(
				&resampler->published_source_format_generation,
				memory_order_relaxed);
		statistics->source_sample_rate = atomic_load_explicit(
				&resampler->published_source_sample_rate,
				memory_order_relaxed);
		statistics->source_channels = atomic_load_explicit(
				&resampler->published_source_channels,
				memory_order_relaxed);
		sequence_after = atomic_load_explicit(
				&resampler->published_source_format_sequence,
				memory_order_acquire);
		if (sequence_before == sequence_after) {
			break;
		}
	}
	statistics->source_format_changes = atomic_load_explicit(
			&resampler->source_format_change_count,
			memory_order_relaxed);
	statistics->source_consumed_bytes = atomic_load_explicit(
			&resampler->source_consumed_byte_count,
			memory_order_relaxed);
	statistics->target_produced_bytes = atomic_load_explicit(
			&resampler->target_produced_byte_count,
			memory_order_relaxed);
	statistics->resampler_creates = atomic_load_explicit(
			&resampler->resampler_create_count,
			memory_order_relaxed);
	statistics->resampler_errors = atomic_load_explicit(
			&resampler->resampler_error_count,
			memory_order_relaxed);
	statistics->source_processing_failed_bytes = atomic_load_explicit(
			&resampler->source_processing_failed_byte_count,
			memory_order_relaxed);
	statistics->pending_target_dropped_bytes = atomic_load_explicit(
			&resampler->pending_target_dropped_byte_count,
			memory_order_relaxed);
}

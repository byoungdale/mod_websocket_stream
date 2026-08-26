#include "playback_timing.h"

#include <string.h>

static bool update_maximum(
		_Atomic uint64_t *maximum,
		uint64_t candidate)
{
	uint64_t current = atomic_load_explicit(maximum, memory_order_relaxed);

	while (candidate > current) {
		if (atomic_compare_exchange_weak_explicit(
				maximum,
				&current,
				candidate,
				memory_order_relaxed,
				memory_order_relaxed)) {
			return true;
		}
	}

	return false;
}

static uint64_t age_us(uint64_t now_us, uint64_t then_us)
{
	return then_us && now_us > then_us ? now_us - then_us : 0;
}

static void atomic_observation_init(
		mws_atomic_playback_timing_observation_t *observation)
{
	atomic_init(&observation->sequence, 0);
	atomic_init(&observation->observed_at_monotonic_us, 0);
	atomic_init(&observation->callback_entry_interval_us, 0);
	atomic_init(&observation->callback_expected_interval_us, 0);
	atomic_init(&observation->callback_lateness_us, 0);
	atomic_init(&observation->module_dispatch_delay_us, 0);
	atomic_init(&observation->source_queued_bytes, 0);
	atomic_init(&observation->playback_queued_bytes, 0);
	atomic_init(&observation->websocket_audio_age_us, 0);
	atomic_init(&observation->playback_buffer_write_age_us, 0);
	atomic_init(&observation->worker_visit_age_us, 0);
	atomic_init(&observation->websocket_audio_seen, 0);
	atomic_init(&observation->playback_buffer_write_seen, 0);
	atomic_init(&observation->worker_visit_seen, 0);
	atomic_init(&observation->playback_state, 0);
}

static void write_observation(
		mws_atomic_playback_timing_observation_t *destination,
		const mws_playback_timing_observation_t *source)
{
	/*
	 * Each stream has one playback direction and therefore one media-callback
	 * writer. An odd sequence marks the atomic fields as being updated; readers
	 * accept a copy only when they observe the same even sequence before and
	 * after it. Reads are bounded so a descheduled media thread cannot stall the
	 * shared transport worker.
	 */
	atomic_fetch_add_explicit(
			&destination->sequence,
			1,
			memory_order_seq_cst);
	atomic_thread_fence(memory_order_seq_cst);
	atomic_store_explicit(
			&destination->observed_at_monotonic_us,
			source->observed_at_monotonic_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->callback_entry_interval_us,
			source->callback_entry_interval_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->callback_expected_interval_us,
			source->callback_expected_interval_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->callback_lateness_us,
			source->callback_lateness_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->module_dispatch_delay_us,
			source->module_dispatch_delay_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->source_queued_bytes,
			source->source_queued_bytes,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->playback_queued_bytes,
			source->playback_queued_bytes,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->websocket_audio_age_us,
			source->websocket_audio_age_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->playback_buffer_write_age_us,
			source->playback_buffer_write_age_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->worker_visit_age_us,
			source->worker_visit_age_us,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->websocket_audio_seen,
			source->websocket_audio_seen ? 1U : 0U,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->playback_buffer_write_seen,
			source->playback_buffer_write_seen ? 1U : 0U,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->worker_visit_seen,
			source->worker_visit_seen ? 1U : 0U,
			memory_order_relaxed);
	atomic_store_explicit(
			&destination->playback_state,
			source->playback_state,
			memory_order_relaxed);
	atomic_thread_fence(memory_order_seq_cst);
	atomic_fetch_add_explicit(
			&destination->sequence,
			1,
			memory_order_seq_cst);
}

static bool read_observation(
		const mws_atomic_playback_timing_observation_t *source,
		mws_playback_timing_observation_t *destination)
{
	uint32_t attempt;
	uint64_t sequence_before;
	uint64_t sequence_after;

	memset(destination, 0, sizeof(*destination));
	for (attempt = 0; attempt < MWS_PLAYBACK_OBSERVATION_READ_ATTEMPTS;
			attempt++) {
		sequence_before = atomic_load_explicit(
				&source->sequence,
				memory_order_seq_cst);
		if (sequence_before & 1) {
			continue;
		}
		if (!sequence_before) {
			return true;
		}
		atomic_thread_fence(memory_order_seq_cst);
		destination->observed_at_monotonic_us = atomic_load_explicit(
				&source->observed_at_monotonic_us,
				memory_order_relaxed);
		destination->callback_entry_interval_us = atomic_load_explicit(
				&source->callback_entry_interval_us,
				memory_order_relaxed);
		destination->callback_expected_interval_us = atomic_load_explicit(
				&source->callback_expected_interval_us,
				memory_order_relaxed);
		destination->callback_lateness_us = atomic_load_explicit(
				&source->callback_lateness_us,
				memory_order_relaxed);
		destination->module_dispatch_delay_us = atomic_load_explicit(
				&source->module_dispatch_delay_us,
				memory_order_relaxed);
		destination->source_queued_bytes = atomic_load_explicit(
				&source->source_queued_bytes,
				memory_order_relaxed);
		destination->playback_queued_bytes = atomic_load_explicit(
				&source->playback_queued_bytes,
				memory_order_relaxed);
		destination->websocket_audio_age_us = atomic_load_explicit(
				&source->websocket_audio_age_us,
				memory_order_relaxed);
		destination->playback_buffer_write_age_us = atomic_load_explicit(
				&source->playback_buffer_write_age_us,
				memory_order_relaxed);
		destination->worker_visit_age_us = atomic_load_explicit(
				&source->worker_visit_age_us,
				memory_order_relaxed);
		destination->websocket_audio_seen = atomic_load_explicit(
				&source->websocket_audio_seen,
				memory_order_relaxed) != 0;
		destination->playback_buffer_write_seen = atomic_load_explicit(
				&source->playback_buffer_write_seen,
				memory_order_relaxed) != 0;
		destination->worker_visit_seen = atomic_load_explicit(
				&source->worker_visit_seen,
				memory_order_relaxed) != 0;
		destination->playback_state = atomic_load_explicit(
				&source->playback_state,
				memory_order_relaxed);
		atomic_thread_fence(memory_order_seq_cst);
		sequence_after = atomic_load_explicit(
				&source->sequence,
				memory_order_seq_cst);
		if (sequence_before == sequence_after && !(sequence_after & 1)) {
			destination->valid = true;
			return true;
		}
	}

	memset(destination, 0, sizeof(*destination));
	return false;
}

static mws_playback_timing_observation_t make_observation(
		const mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t source_queued_bytes,
		uint64_t playback_queued_bytes,
		uint32_t playback_state)
{
	mws_playback_timing_observation_t observation;
	uint64_t last_buffer_write_at_us;
	uint64_t last_websocket_audio_at_us;
	uint64_t last_worker_visit_at_us;

	memset(&observation, 0, sizeof(observation));
	observation.valid = true;
	observation.observed_at_monotonic_us = dispatch_at_us;
	if (measurement) {
		observation.callback_entry_interval_us = measurement->interval_us;
		observation.callback_expected_interval_us =
				measurement->expected_interval_us;
		observation.callback_lateness_us = measurement->lateness_us;
		if (dispatch_at_us > measurement->entry_at_us) {
			observation.module_dispatch_delay_us =
					dispatch_at_us - measurement->entry_at_us;
		}
	}
	observation.source_queued_bytes = source_queued_bytes;
	observation.playback_queued_bytes = playback_queued_bytes;
	last_websocket_audio_at_us = atomic_load_explicit(
			&timing->last_websocket_audio_at_us,
			memory_order_acquire);
	observation.websocket_audio_seen = last_websocket_audio_at_us != 0 &&
			last_websocket_audio_at_us <= dispatch_at_us;
	observation.websocket_audio_age_us = age_us(
			dispatch_at_us,
			last_websocket_audio_at_us);
	last_buffer_write_at_us = atomic_load_explicit(
			&timing->last_playback_buffer_write_at_us,
			memory_order_acquire);
	observation.playback_buffer_write_seen = last_buffer_write_at_us != 0 &&
			last_buffer_write_at_us <= dispatch_at_us;
	observation.playback_buffer_write_age_us = age_us(
			dispatch_at_us,
			last_buffer_write_at_us);
	last_worker_visit_at_us = atomic_load_explicit(
			&timing->last_worker_visit_at_us,
			memory_order_acquire);
	observation.worker_visit_seen = last_worker_visit_at_us != 0 &&
			last_worker_visit_at_us <= dispatch_at_us;
	observation.worker_visit_age_us = age_us(
			dispatch_at_us,
			last_worker_visit_at_us);
	observation.playback_state = playback_state;

	return observation;
}

void mws_media_callback_clock_init(mws_media_callback_clock_t *clock)
{
	atomic_init(&clock->previous_entry_at_us, 0);
	atomic_init(&clock->previous_expected_interval_us, 0);
}

uint64_t mws_playback_frame_duration_us(
		uint64_t sample_count,
		uint32_t sample_rate)
{
	if (!sample_count || !sample_rate ||
			sample_count > UINT64_MAX / 1000000ULL) {
		return 0;
	}

	return (sample_count * 1000000ULL + sample_rate / 2) / sample_rate;
}

mws_media_callback_measurement_t mws_media_callback_clock_measure(
		mws_media_callback_clock_t *clock,
		uint64_t entry_at_us,
		uint64_t next_expected_interval_us)
{
	mws_media_callback_measurement_t measurement;
	uint64_t previous_entry_at_us;
	uint64_t previous_expected_interval_us;

	memset(&measurement, 0, sizeof(measurement));
	measurement.entry_at_us = entry_at_us;
	previous_entry_at_us = atomic_exchange_explicit(
			&clock->previous_entry_at_us,
			entry_at_us,
			memory_order_relaxed);
	previous_expected_interval_us = atomic_exchange_explicit(
			&clock->previous_expected_interval_us,
			next_expected_interval_us,
			memory_order_relaxed);
	if (!previous_entry_at_us || !previous_expected_interval_us ||
			entry_at_us <= previous_entry_at_us) {
		return measurement;
	}

	measurement.interval_available = true;
	measurement.interval_us = entry_at_us - previous_entry_at_us;
	measurement.expected_interval_us = previous_expected_interval_us;
	if (measurement.interval_us > measurement.expected_interval_us) {
		measurement.lateness_us = measurement.interval_us -
				measurement.expected_interval_us;
	}

	return measurement;
}

void mws_playback_timing_init(mws_playback_timing_t *timing)
{
	atomic_init(&timing->callback_count, 0);
	atomic_init(&timing->callback_requested_samples, 0);
	atomic_init(&timing->callback_interval_count, 0);
	atomic_init(&timing->callback_interval_total_us, 0);
	atomic_init(&timing->callback_maximum_interval_us, 0);
	atomic_init(&timing->callback_maximum_lateness_us, 0);
	atomic_init(&timing->callback_gap_interval_count, 0);
	atomic_init(&timing->module_dispatch_maximum_delay_us, 0);
	atomic_init(&timing->underflow_transition_count, 0);
	atomic_init(&timing->underflow_with_source_audio_count, 0);
	atomic_init(&timing->observation_snapshot_miss_count, 0);
	atomic_init(&timing->last_websocket_audio_at_us, 0);
	atomic_init(&timing->last_playback_buffer_write_at_us, 0);
	atomic_init(&timing->last_worker_visit_at_us, 0);
	atomic_observation_init(&timing->maximum_gap);
	atomic_observation_init(&timing->last_underflow);
}

void mws_playback_timing_note_websocket_audio(
		mws_playback_timing_t *timing,
		uint64_t now_us)
{
	atomic_store_explicit(
			&timing->last_websocket_audio_at_us,
			now_us,
			memory_order_release);
}

void mws_playback_timing_note_playback_buffer_write(
		mws_playback_timing_t *timing,
		uint64_t now_us)
{
	atomic_store_explicit(
			&timing->last_playback_buffer_write_at_us,
			now_us,
			memory_order_release);
}

void mws_playback_timing_note_worker_visit(
		mws_playback_timing_t *timing,
		uint64_t now_us)
{
	atomic_store_explicit(
			&timing->last_worker_visit_at_us,
			now_us,
			memory_order_release);
}

bool mws_playback_timing_record_callback(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t requested_samples,
		bool playback_active)
{
	uint64_t dispatch_delay_us = 0;
	uint64_t previous_callback_count;
	bool maximum_gap_changed = false;

	previous_callback_count = atomic_fetch_add_explicit(
			&timing->callback_count,
			1,
			memory_order_relaxed);
	atomic_fetch_add_explicit(
			&timing->callback_requested_samples,
			requested_samples,
			memory_order_relaxed);
	if (measurement && dispatch_at_us > measurement->entry_at_us) {
		dispatch_delay_us = dispatch_at_us - measurement->entry_at_us;
	}
	update_maximum(
			&timing->module_dispatch_maximum_delay_us,
			dispatch_delay_us);

	if (!previous_callback_count || !measurement ||
			!measurement->interval_available) {
		return false;
	}

	atomic_fetch_add_explicit(
			&timing->callback_interval_count,
			1,
			memory_order_relaxed);
	atomic_fetch_add_explicit(
			&timing->callback_interval_total_us,
			measurement->interval_us,
			memory_order_relaxed);
	update_maximum(
			&timing->callback_maximum_interval_us,
			measurement->interval_us);
	if (!playback_active) {
		return false;
	}
	maximum_gap_changed = update_maximum(
			&timing->callback_maximum_lateness_us,
			measurement->lateness_us);
	if (measurement->lateness_us >
			MWS_PLAYBACK_CALLBACK_GAP_LATENESS_US) {
		atomic_fetch_add_explicit(
				&timing->callback_gap_interval_count,
				1,
				memory_order_relaxed);
		return maximum_gap_changed;
	}

	return false;
}

void mws_playback_timing_record_maximum_gap(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t source_queued_bytes,
		uint64_t playback_queued_bytes,
		uint32_t playback_state)
{
	mws_playback_timing_observation_t observation = make_observation(
			timing,
			measurement,
			dispatch_at_us,
			source_queued_bytes,
			playback_queued_bytes,
			playback_state);

	write_observation(&timing->maximum_gap, &observation);
}

void mws_playback_timing_record_underflow(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t source_queued_bytes,
		uint64_t playback_queued_bytes,
		uint32_t playback_state)
{
	mws_playback_timing_observation_t observation = make_observation(
			timing,
			measurement,
			dispatch_at_us,
			source_queued_bytes,
			playback_queued_bytes,
			playback_state);

	atomic_fetch_add_explicit(
			&timing->underflow_transition_count,
			1,
			memory_order_relaxed);
	if (source_queued_bytes) {
		atomic_fetch_add_explicit(
				&timing->underflow_with_source_audio_count,
				1,
				memory_order_relaxed);
	}
	write_observation(&timing->last_underflow, &observation);
}

void mws_playback_timing_snapshot(
		mws_playback_timing_t *timing,
		uint64_t now_us,
		mws_playback_timing_statistics_t *statistics)
{
	uint64_t last_buffer_write_at_us;
	uint64_t last_websocket_audio_at_us;
	uint64_t last_worker_visit_at_us;

	memset(statistics, 0, sizeof(*statistics));
	statistics->callback_count = atomic_load_explicit(
			&timing->callback_count,
			memory_order_relaxed);
	statistics->callback_requested_samples = atomic_load_explicit(
			&timing->callback_requested_samples,
			memory_order_relaxed);
	statistics->callback_interval_count = atomic_load_explicit(
			&timing->callback_interval_count,
			memory_order_relaxed);
	statistics->callback_interval_total_us = atomic_load_explicit(
			&timing->callback_interval_total_us,
			memory_order_relaxed);
	statistics->callback_maximum_interval_us = atomic_load_explicit(
			&timing->callback_maximum_interval_us,
			memory_order_relaxed);
	statistics->callback_maximum_lateness_us = atomic_load_explicit(
			&timing->callback_maximum_lateness_us,
			memory_order_relaxed);
	statistics->callback_gap_interval_count = atomic_load_explicit(
			&timing->callback_gap_interval_count,
			memory_order_relaxed);
	statistics->module_dispatch_maximum_delay_us = atomic_load_explicit(
			&timing->module_dispatch_maximum_delay_us,
			memory_order_relaxed);
	statistics->underflow_transition_count = atomic_load_explicit(
			&timing->underflow_transition_count,
			memory_order_relaxed);
	statistics->underflow_with_source_audio_count = atomic_load_explicit(
			&timing->underflow_with_source_audio_count,
			memory_order_relaxed);
	statistics->observation_snapshot_miss_count = atomic_load_explicit(
			&timing->observation_snapshot_miss_count,
			memory_order_relaxed);
	last_websocket_audio_at_us = atomic_load_explicit(
			&timing->last_websocket_audio_at_us,
			memory_order_acquire);
	last_buffer_write_at_us = atomic_load_explicit(
			&timing->last_playback_buffer_write_at_us,
			memory_order_acquire);
	last_worker_visit_at_us = atomic_load_explicit(
			&timing->last_worker_visit_at_us,
			memory_order_acquire);
	statistics->websocket_audio_seen = last_websocket_audio_at_us != 0 &&
			last_websocket_audio_at_us <= now_us;
	statistics->playback_buffer_write_seen = last_buffer_write_at_us != 0 &&
			last_buffer_write_at_us <= now_us;
	statistics->worker_visit_seen = last_worker_visit_at_us != 0 &&
			last_worker_visit_at_us <= now_us;
	statistics->websocket_audio_age_us = age_us(
			now_us,
			last_websocket_audio_at_us);
	statistics->playback_buffer_write_age_us = age_us(
			now_us,
			last_buffer_write_at_us);
	statistics->worker_visit_age_us = age_us(
			now_us,
			last_worker_visit_at_us);
	if (!read_observation(&timing->maximum_gap, &statistics->maximum_gap)) {
		atomic_fetch_add_explicit(
				&timing->observation_snapshot_miss_count,
				1,
				memory_order_relaxed);
		statistics->observation_snapshot_miss_count++;
	}
	if (!read_observation(&timing->last_underflow, &statistics->last_underflow)) {
		atomic_fetch_add_explicit(
				&timing->observation_snapshot_miss_count,
				1,
				memory_order_relaxed);
		statistics->observation_snapshot_miss_count++;
	}
	statistics->maximum_gap.observation_age_us = age_us(
			now_us,
			statistics->maximum_gap.observed_at_monotonic_us);
	statistics->last_underflow.observation_age_us = age_us(
			now_us,
			statistics->last_underflow.observed_at_monotonic_us);
}

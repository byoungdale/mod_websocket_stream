#ifndef MWS_PLAYBACK_TIMING_H
#define MWS_PLAYBACK_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* Classification only: recording a gap never changes playback or fires an event. */
#define MWS_PLAYBACK_CALLBACK_GAP_LATENESS_US 100000ULL
#define MWS_PLAYBACK_OBSERVATION_READ_ATTEMPTS 8

typedef struct {
	_Atomic uint64_t previous_entry_at_us;
	_Atomic uint64_t previous_expected_interval_us;
} mws_media_callback_clock_t;

typedef struct {
	bool interval_available;
	uint64_t entry_at_us;
	uint64_t interval_us;
	uint64_t expected_interval_us;
	uint64_t lateness_us;
} mws_media_callback_measurement_t;

typedef struct {
	bool valid;
	bool websocket_audio_seen;
	bool playback_buffer_write_seen;
	bool worker_visit_seen;
	uint64_t observed_at_monotonic_us;
	uint64_t observation_age_us;
	uint64_t callback_entry_interval_us;
	uint64_t callback_expected_interval_us;
	uint64_t callback_lateness_us;
	uint64_t module_dispatch_delay_us;
	uint64_t source_queued_bytes;
	uint64_t playback_queued_bytes;
	uint64_t websocket_audio_age_us;
	uint64_t playback_buffer_write_age_us;
	uint64_t worker_visit_age_us;
	uint32_t playback_state;
} mws_playback_timing_observation_t;

typedef struct {
	_Atomic uint64_t sequence;
	_Atomic uint64_t observed_at_monotonic_us;
	_Atomic uint64_t callback_entry_interval_us;
	_Atomic uint64_t callback_expected_interval_us;
	_Atomic uint64_t callback_lateness_us;
	_Atomic uint64_t module_dispatch_delay_us;
	_Atomic uint64_t source_queued_bytes;
	_Atomic uint64_t playback_queued_bytes;
	_Atomic uint64_t websocket_audio_age_us;
	_Atomic uint64_t playback_buffer_write_age_us;
	_Atomic uint64_t worker_visit_age_us;
	_Atomic uint32_t websocket_audio_seen;
	_Atomic uint32_t playback_buffer_write_seen;
	_Atomic uint32_t worker_visit_seen;
	_Atomic uint32_t playback_state;
} mws_atomic_playback_timing_observation_t;

typedef struct {
	_Atomic uint64_t callback_count;
	_Atomic uint64_t callback_requested_samples;
	_Atomic uint64_t callback_interval_count;
	_Atomic uint64_t callback_interval_total_us;
	_Atomic uint64_t callback_maximum_interval_us;
	_Atomic uint64_t callback_maximum_lateness_us;
	_Atomic uint64_t callback_gap_interval_count;
	_Atomic uint64_t module_dispatch_maximum_delay_us;
	_Atomic uint64_t underflow_transition_count;
	_Atomic uint64_t underflow_with_source_audio_count;
	_Atomic uint64_t observation_snapshot_miss_count;
	_Atomic uint64_t last_websocket_audio_at_us;
	_Atomic uint64_t last_playback_buffer_write_at_us;
	_Atomic uint64_t last_worker_visit_at_us;
	mws_atomic_playback_timing_observation_t maximum_gap;
	mws_atomic_playback_timing_observation_t last_underflow;
} mws_playback_timing_t;

typedef struct {
	uint64_t callback_count;
	uint64_t callback_requested_samples;
	uint64_t callback_interval_count;
	uint64_t callback_interval_total_us;
	uint64_t callback_maximum_interval_us;
	uint64_t callback_maximum_lateness_us;
	uint64_t callback_gap_interval_count;
	uint64_t module_dispatch_maximum_delay_us;
	uint64_t underflow_transition_count;
	uint64_t underflow_with_source_audio_count;
	uint64_t observation_snapshot_miss_count;
	bool websocket_audio_seen;
	bool playback_buffer_write_seen;
	bool worker_visit_seen;
	uint64_t websocket_audio_age_us;
	uint64_t playback_buffer_write_age_us;
	uint64_t worker_visit_age_us;
	mws_playback_timing_observation_t maximum_gap;
	mws_playback_timing_observation_t last_underflow;
} mws_playback_timing_statistics_t;

void mws_media_callback_clock_init(mws_media_callback_clock_t *clock);

uint64_t mws_playback_frame_duration_us(
		uint64_t sample_count,
		uint32_t sample_rate);

mws_media_callback_measurement_t mws_media_callback_clock_measure(
		mws_media_callback_clock_t *clock,
		uint64_t entry_at_us,
		uint64_t next_expected_interval_us);

void mws_playback_timing_init(mws_playback_timing_t *timing);

void mws_playback_timing_note_websocket_audio(
		mws_playback_timing_t *timing,
		uint64_t now_us);

void mws_playback_timing_note_playback_buffer_write(
		mws_playback_timing_t *timing,
		uint64_t now_us);

void mws_playback_timing_note_worker_visit(
		mws_playback_timing_t *timing,
		uint64_t now_us);

bool mws_playback_timing_record_callback(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t requested_samples,
		bool playback_active);

void mws_playback_timing_record_maximum_gap(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t source_queued_bytes,
		uint64_t playback_queued_bytes,
		uint32_t playback_state);

void mws_playback_timing_record_underflow(
		mws_playback_timing_t *timing,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t source_queued_bytes,
		uint64_t playback_queued_bytes,
		uint32_t playback_state);

void mws_playback_timing_snapshot(
		mws_playback_timing_t *timing,
		uint64_t now_us,
		mws_playback_timing_statistics_t *statistics);

#endif

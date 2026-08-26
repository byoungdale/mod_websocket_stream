#ifndef MWS_PLAYBACK_RATE_CONTROLLER_H
#define MWS_PLAYBACK_RATE_CONTROLLER_H

#include <stdint.h>

#define MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM 10000

typedef struct {
	int64_t target_queue_us;
	int64_t filtered_queue_us;
	int64_t integral_error_us;
	int32_t adjustment_ppm;
	uint32_t maximum_adjustment_ppm;
	uint64_t update_count;
	uint64_t limit_count;
	uint64_t queued_audio_total_us;
	uint32_t queue_observation_count;
} mws_playback_rate_controller_t;

void mws_playback_rate_controller_init(
		mws_playback_rate_controller_t *controller,
		int64_t target_queue_us);

void mws_playback_rate_controller_reset(
		mws_playback_rate_controller_t *controller);

int32_t mws_playback_rate_controller_update(
		mws_playback_rate_controller_t *controller,
		int64_t queued_audio_us);

void mws_playback_rate_controller_observe_queue(
		mws_playback_rate_controller_t *controller,
		uint64_t queued_audio_us);

int32_t mws_playback_rate_controller_update_from_observations(
		mws_playback_rate_controller_t *controller);

uint64_t mws_playback_queued_duration_us(
		uint64_t source_sample_count,
		uint32_t source_sample_rate,
		uint64_t playback_sample_count,
		uint32_t playback_sample_rate);

#endif

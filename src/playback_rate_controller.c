#include "playback_rate_controller.h"

#define MWS_PLAYBACK_RATE_FILTER_WEIGHT 8
#define MWS_PLAYBACK_RATE_DEADBAND_US 5000
#define MWS_PLAYBACK_RATE_PROPORTIONAL_DIVISOR 10
#define MWS_PLAYBACK_RATE_INTEGRAL_DIVISOR 1000
#define MWS_PLAYBACK_RATE_MAX_STEP_PPM 2000
#define MWS_PLAYBACK_RATE_RESOLUTION_PPM 100

static int64_t clamp_value(int64_t value, int64_t minimum, int64_t maximum)
{
	if (value < minimum) {
		return minimum;
	}
	if (value > maximum) {
		return maximum;
	}
	return value;
}

static int32_t round_adjustment(int64_t adjustment_ppm)
{
	if (adjustment_ppm >= 0) {
		return (int32_t) (
				(adjustment_ppm + MWS_PLAYBACK_RATE_RESOLUTION_PPM / 2) /
				MWS_PLAYBACK_RATE_RESOLUTION_PPM *
				MWS_PLAYBACK_RATE_RESOLUTION_PPM);
	}

	return (int32_t) -(
			(-adjustment_ppm + MWS_PLAYBACK_RATE_RESOLUTION_PPM / 2) /
			MWS_PLAYBACK_RATE_RESOLUTION_PPM *
			MWS_PLAYBACK_RATE_RESOLUTION_PPM);
}

void mws_playback_rate_controller_init(
		mws_playback_rate_controller_t *controller,
		int64_t target_queue_us)
{
	controller->target_queue_us = target_queue_us;
	controller->maximum_adjustment_ppm = 0;
	controller->update_count = 0;
	controller->limit_count = 0;
	mws_playback_rate_controller_reset(controller);
}

void mws_playback_rate_controller_reset(
		mws_playback_rate_controller_t *controller)
{
	controller->filtered_queue_us = controller->target_queue_us;
	controller->integral_error_us = 0;
	controller->adjustment_ppm = 0;
	controller->queued_audio_total_us = 0;
	controller->queue_observation_count = 0;
}

int32_t mws_playback_rate_controller_update(
		mws_playback_rate_controller_t *controller,
		int64_t queued_audio_us)
{
	const int64_t integral_limit =
			MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM *
			MWS_PLAYBACK_RATE_INTEGRAL_DIVISOR;
	int64_t queue_error_us;
	int64_t candidate_integral_error_us;
	int64_t proportional_adjustment_ppm;
	int64_t integral_adjustment_ppm;
	int64_t desired_adjustment_ppm;
	int64_t adjustment_step_ppm;
	int32_t next_adjustment_ppm;
	uint32_t absolute_adjustment_ppm;

	/* Smooth ordinary WebSocket batching before it reaches the controller. */
	controller->filtered_queue_us +=
			(queued_audio_us - controller->filtered_queue_us) /
			MWS_PLAYBACK_RATE_FILTER_WEIGHT;
	queue_error_us = controller->filtered_queue_us -
			controller->target_queue_us;
	if (queue_error_us > -MWS_PLAYBACK_RATE_DEADBAND_US &&
			queue_error_us < MWS_PLAYBACK_RATE_DEADBAND_US) {
		queue_error_us = 0;
	}

	proportional_adjustment_ppm = queue_error_us /
			MWS_PLAYBACK_RATE_PROPORTIONAL_DIVISOR;
	candidate_integral_error_us = clamp_value(
			controller->integral_error_us + queue_error_us,
			-integral_limit,
			integral_limit);
	integral_adjustment_ppm = candidate_integral_error_us /
			MWS_PLAYBACK_RATE_INTEGRAL_DIVISOR;
	desired_adjustment_ppm = proportional_adjustment_ppm +
			integral_adjustment_ppm;

	/* Do not accumulate more integral error while pushing into a limit. */
	if ((desired_adjustment_ppm >
			MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM && queue_error_us > 0) ||
			(desired_adjustment_ppm <
			-MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM && queue_error_us < 0)) {
		candidate_integral_error_us = controller->integral_error_us;
		integral_adjustment_ppm = candidate_integral_error_us /
				MWS_PLAYBACK_RATE_INTEGRAL_DIVISOR;
		desired_adjustment_ppm = proportional_adjustment_ppm +
				integral_adjustment_ppm;
	}
	controller->integral_error_us = candidate_integral_error_us;

	desired_adjustment_ppm = clamp_value(
			desired_adjustment_ppm,
			-MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM,
			MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM);
	adjustment_step_ppm = clamp_value(
			desired_adjustment_ppm - controller->adjustment_ppm,
			-MWS_PLAYBACK_RATE_MAX_STEP_PPM,
			MWS_PLAYBACK_RATE_MAX_STEP_PPM);
	next_adjustment_ppm = round_adjustment(
			controller->adjustment_ppm + adjustment_step_ppm);
	next_adjustment_ppm = (int32_t) clamp_value(
			next_adjustment_ppm,
			-MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM,
			MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM);

	controller->adjustment_ppm = next_adjustment_ppm;
	absolute_adjustment_ppm = (uint32_t) (next_adjustment_ppm < 0 ?
			-next_adjustment_ppm : next_adjustment_ppm);
	if (absolute_adjustment_ppm > controller->maximum_adjustment_ppm) {
		controller->maximum_adjustment_ppm = absolute_adjustment_ppm;
	}
	controller->update_count++;
	if (next_adjustment_ppm == MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM ||
			next_adjustment_ppm ==
			-MWS_PLAYBACK_RATE_MAX_ADJUSTMENT_PPM) {
		controller->limit_count++;
	}

	return next_adjustment_ppm;
}

void mws_playback_rate_controller_observe_queue(
		mws_playback_rate_controller_t *controller,
		uint64_t queued_audio_us)
{
	/* Average across the control interval so frame batches are not mistaken
	 * for sustained clock drift. */
	controller->queued_audio_total_us += queued_audio_us;
	controller->queue_observation_count++;
}

int32_t mws_playback_rate_controller_update_from_observations(
		mws_playback_rate_controller_t *controller)
{
	uint64_t average_queued_audio_us;

	if (!controller->queue_observation_count) {
		return controller->adjustment_ppm;
	}

	average_queued_audio_us = controller->queued_audio_total_us /
			controller->queue_observation_count;
	controller->queued_audio_total_us = 0;
	controller->queue_observation_count = 0;

	return mws_playback_rate_controller_update(
			controller,
			(int64_t) average_queued_audio_us);
}

static uint64_t sample_count_to_duration_us(
		uint64_t sample_count,
		uint32_t sample_rate)
{
	if (!sample_rate) {
		return 0;
	}

	return sample_count / sample_rate * 1000000 +
			(sample_count % sample_rate) * 1000000 / sample_rate;
}

uint64_t mws_playback_queued_duration_us(
		uint64_t source_sample_count,
		uint32_t source_sample_rate,
		uint64_t playback_sample_count,
		uint32_t playback_sample_rate)
{
	return sample_count_to_duration_us(
				source_sample_count,
				source_sample_rate) +
			sample_count_to_duration_us(
				playback_sample_count,
				playback_sample_rate);
}

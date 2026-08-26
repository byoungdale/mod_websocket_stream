#include "websocket_audio_input.h"

#include <limits.h>
#include <string.h>

static void convert_little_endian_pcm_to_native(
		uint8_t *data,
		uint32_t data_length)
{
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
	uint32_t offset;

	for (offset = 0; offset < data_length; offset += sizeof(int16_t)) {
		uint8_t first_byte = data[offset];

		data[offset] = data[offset + 1];
		data[offset + 1] = first_byte;
	}
#else
	(void) data;
	(void) data_length;
#endif
}

switch_status_t mws_websocket_audio_input_initialize(
		mws_websocket_audio_input_t *audio_input,
		uint8_t *conversion_buffer,
		uint32_t conversion_capacity_bytes)
{
	if (!audio_input || !conversion_buffer ||
			conversion_capacity_bytes < sizeof(int16_t) ||
			conversion_capacity_bytes % sizeof(int16_t)) {
		return SWITCH_STATUS_FALSE;
	}

	memset(audio_input, 0, sizeof(*audio_input));
	audio_input->conversion_buffer = conversion_buffer;
	audio_input->conversion_capacity_bytes = conversion_capacity_bytes;
	return SWITCH_STATUS_SUCCESS;
}

mws_websocket_audio_input_result_t mws_websocket_audio_input_write(
		mws_websocket_audio_input_t *audio_input,
		mws_source_audio_buffer_t *source_audio,
		const uint8_t *data,
		switch_size_t data_length,
		mws_websocket_audio_input_write_result_t *write_result)
{
	uint32_t completed_length_bytes;

	if (write_result) {
		memset(write_result, 0, sizeof(*write_result));
	}
	if (!audio_input || !source_audio || !write_result ||
			(data_length && !data) ||
			data_length > UINT32_MAX -
					(audio_input->partial_sample_ready ? 1U : 0U)) {
		return MWS_WEBSOCKET_AUDIO_INPUT_ERROR;
	}
	if (audio_input->discard_until_frame_end) {
		return MWS_WEBSOCKET_AUDIO_INPUT_ACCEPTED;
	}

	completed_length_bytes = (uint32_t) data_length +
			(audio_input->partial_sample_ready ? 1U : 0U);
	completed_length_bytes -= completed_length_bytes % sizeof(int16_t);
	if (completed_length_bytes > mws_source_audio_buffer_free_bytes(
			source_audio)) {
		write_result->required_free_bytes = completed_length_bytes;
		return MWS_WEBSOCKET_AUDIO_INPUT_FULL;
	}

	while (data_length) {
		switch_size_t copy_length;

		if (audio_input->partial_sample_ready) {
			audio_input->conversion_buffer[0] =
					audio_input->partial_sample_byte;
			audio_input->conversion_buffer[1] = *data;
			audio_input->partial_sample_ready = SWITCH_FALSE;
			data++;
			data_length--;
			convert_little_endian_pcm_to_native(
					audio_input->conversion_buffer,
					sizeof(int16_t));
			mws_source_audio_buffer_write(
					source_audio,
					audio_input->conversion_buffer,
					sizeof(int16_t));
		}

		copy_length = data_length - data_length % sizeof(int16_t);
		if (copy_length > audio_input->conversion_capacity_bytes) {
			copy_length = audio_input->conversion_capacity_bytes;
		}

		if (copy_length) {
			memcpy(audio_input->conversion_buffer, data, copy_length);
			convert_little_endian_pcm_to_native(
					audio_input->conversion_buffer,
					(uint32_t) copy_length);
			mws_source_audio_buffer_write(
					source_audio,
					audio_input->conversion_buffer,
					(uint32_t) copy_length);
			data += copy_length;
			data_length -= copy_length;
		}

		if (data_length == 1) {
			audio_input->partial_sample_byte = *data;
			audio_input->partial_sample_ready = SWITCH_TRUE;
			data_length = 0;
		}
	}

	write_result->accepted_sample_count =
			completed_length_bytes / sizeof(int16_t);
	return MWS_WEBSOCKET_AUDIO_INPUT_ACCEPTED;
}

switch_bool_t mws_websocket_audio_input_finish_frame(
		mws_websocket_audio_input_t *audio_input)
{
	if (audio_input && audio_input->discard_until_frame_end) {
		audio_input->discard_until_frame_end = SWITCH_FALSE;
		return SWITCH_TRUE;
	}
	if (!audio_input || audio_input->partial_sample_ready) {
		mws_websocket_audio_input_reset(audio_input);
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

void mws_websocket_audio_input_clear(
		mws_websocket_audio_input_t *audio_input)
{
	if (!audio_input) {
		return;
	}

	if (audio_input->partial_sample_ready) {
		audio_input->partial_sample_byte = 0;
		audio_input->partial_sample_ready = SWITCH_FALSE;
		audio_input->discard_until_frame_end = SWITCH_TRUE;
	}
}

void mws_websocket_audio_input_reset(
		mws_websocket_audio_input_t *audio_input)
{
	if (!audio_input) {
		return;
	}

	audio_input->partial_sample_byte = 0;
	audio_input->partial_sample_ready = SWITCH_FALSE;
	audio_input->discard_until_frame_end = SWITCH_FALSE;
}

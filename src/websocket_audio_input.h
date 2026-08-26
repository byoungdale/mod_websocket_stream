#ifndef MWS_WEBSOCKET_AUDIO_INPUT_H
#define MWS_WEBSOCKET_AUDIO_INPUT_H

#include <switch.h>

#include <stdint.h>

#include "source_audio_buffer.h"

typedef enum {
	MWS_WEBSOCKET_AUDIO_INPUT_ERROR = -1,
	MWS_WEBSOCKET_AUDIO_INPUT_ACCEPTED = 0,
	MWS_WEBSOCKET_AUDIO_INPUT_FULL = 1
} mws_websocket_audio_input_result_t;

typedef struct {
	uint32_t accepted_sample_count;
	uint32_t required_free_bytes;
} mws_websocket_audio_input_write_result_t;

typedef struct {
	uint8_t *conversion_buffer;
	uint32_t conversion_capacity_bytes;
	uint8_t partial_sample_byte;
	switch_bool_t partial_sample_ready;
	switch_bool_t discard_until_frame_end;
} mws_websocket_audio_input_t;

switch_status_t mws_websocket_audio_input_initialize(
		mws_websocket_audio_input_t *audio_input,
		uint8_t *conversion_buffer,
		uint32_t conversion_capacity_bytes);

mws_websocket_audio_input_result_t mws_websocket_audio_input_write(
		mws_websocket_audio_input_t *audio_input,
		mws_source_audio_buffer_t *source_audio,
		const uint8_t *data,
		switch_size_t data_length,
		mws_websocket_audio_input_write_result_t *write_result);

switch_bool_t mws_websocket_audio_input_finish_frame(
		mws_websocket_audio_input_t *audio_input);

void mws_websocket_audio_input_clear(
		mws_websocket_audio_input_t *audio_input);

void mws_websocket_audio_input_reset(
		mws_websocket_audio_input_t *audio_input);

#endif

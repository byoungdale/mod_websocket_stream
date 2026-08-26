#ifndef MWS_PLAYBACK_AUDIO_BUFFER_H
#define MWS_PLAYBACK_AUDIO_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;
	uint32_t allocation_bytes;
	_Atomic uint32_t capacity_bytes;
	_Atomic uint32_t read_offset_bytes;
	_Atomic uint32_t write_offset_bytes;
	_Atomic uint32_t maintenance_requested;
	_Atomic uint32_t consumer_active;
} mws_playback_audio_buffer_t;

void mws_playback_audio_buffer_initialize(
		mws_playback_audio_buffer_t *buffer,
		uint8_t *storage,
		uint32_t allocation_bytes,
		uint32_t capacity_bytes);

uint32_t mws_playback_audio_buffer_capacity_bytes(
		const mws_playback_audio_buffer_t *buffer);

uint32_t mws_playback_audio_buffer_queued_bytes(
		const mws_playback_audio_buffer_t *buffer);

bool mws_playback_audio_buffer_begin_read(
		mws_playback_audio_buffer_t *buffer,
		uint32_t *available_bytes);

void mws_playback_audio_buffer_end_read(
		mws_playback_audio_buffer_t *buffer);

void mws_playback_audio_buffer_mix_and_consume(
		mws_playback_audio_buffer_t *buffer,
		int16_t *interleaved_pcm,
		uint32_t sample_count,
		uint32_t channel_count);

void mws_playback_audio_buffer_request_maintenance(
		mws_playback_audio_buffer_t *buffer);

bool mws_playback_audio_buffer_consumer_is_active(
		const mws_playback_audio_buffer_t *buffer);

void mws_playback_audio_buffer_finish_maintenance(
		mws_playback_audio_buffer_t *buffer);

uint32_t mws_playback_audio_buffer_discard_oldest_during_maintenance(
		mws_playback_audio_buffer_t *buffer,
		uint32_t data_length);

uint32_t mws_playback_audio_buffer_discard_all_during_maintenance(
		mws_playback_audio_buffer_t *buffer);

uint32_t mws_playback_audio_buffer_reset_during_maintenance(
		mws_playback_audio_buffer_t *buffer,
		uint32_t capacity_bytes);

void mws_playback_audio_buffer_write(
		mws_playback_audio_buffer_t *buffer,
		const uint8_t *data,
		uint32_t data_length);

#endif

#ifndef MWS_SOURCE_AUDIO_BUFFER_H
#define MWS_SOURCE_AUDIO_BUFFER_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;
	uint32_t capacity_bytes;
	uint32_t read_offset_bytes;
	uint32_t write_offset_bytes;
	_Atomic uint32_t used_bytes;
} mws_source_audio_buffer_t;

void mws_source_audio_buffer_initialize(
		mws_source_audio_buffer_t *buffer,
		uint8_t *storage,
		uint32_t capacity_bytes);

uint32_t mws_source_audio_buffer_queued_bytes(
		const mws_source_audio_buffer_t *buffer);

uint32_t mws_source_audio_buffer_free_bytes(
		const mws_source_audio_buffer_t *buffer);

void mws_source_audio_buffer_write(
		mws_source_audio_buffer_t *buffer,
		const uint8_t *data,
		uint32_t data_length);

uint32_t mws_source_audio_buffer_copy(
		const mws_source_audio_buffer_t *buffer,
		uint8_t *destination,
		uint32_t destination_capacity_bytes);

void mws_source_audio_buffer_consume(
		mws_source_audio_buffer_t *buffer,
		uint32_t data_length);

uint32_t mws_source_audio_buffer_discard_all(
		mws_source_audio_buffer_t *buffer);

#endif

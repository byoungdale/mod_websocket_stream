#include "source_audio_buffer.h"

#include <string.h>

void mws_source_audio_buffer_initialize(
		mws_source_audio_buffer_t *buffer,
		uint8_t *storage,
		uint32_t capacity_bytes)
{
	buffer->data = storage;
	buffer->capacity_bytes = capacity_bytes;
	buffer->read_offset_bytes = 0;
	buffer->write_offset_bytes = 0;
	atomic_init(&buffer->used_bytes, 0);
}

uint32_t mws_source_audio_buffer_queued_bytes(
		const mws_source_audio_buffer_t *buffer)
{
	return atomic_load_explicit(
			&buffer->used_bytes,
			memory_order_acquire);
}

uint32_t mws_source_audio_buffer_free_bytes(
		const mws_source_audio_buffer_t *buffer)
{
	return buffer->capacity_bytes -
			mws_source_audio_buffer_queued_bytes(buffer);
}

void mws_source_audio_buffer_write(
		mws_source_audio_buffer_t *buffer,
		const uint8_t *data,
		uint32_t data_length)
{
	uint32_t used_bytes = mws_source_audio_buffer_queued_bytes(buffer);
	uint32_t first_copy_length = data_length;
	uint32_t bytes_to_end = buffer->capacity_bytes -
			buffer->write_offset_bytes;

	if (first_copy_length > bytes_to_end) {
		first_copy_length = bytes_to_end;
	}

	memcpy(
			buffer->data + buffer->write_offset_bytes,
			data,
			first_copy_length);
	if (first_copy_length < data_length) {
		memcpy(
				buffer->data,
				data + first_copy_length,
				data_length - first_copy_length);
	}

	buffer->write_offset_bytes =
			(buffer->write_offset_bytes + data_length) %
			buffer->capacity_bytes;
	atomic_store_explicit(
			&buffer->used_bytes,
			used_bytes + data_length,
			memory_order_release);
}

uint32_t mws_source_audio_buffer_copy(
		const mws_source_audio_buffer_t *buffer,
		uint8_t *destination,
		uint32_t destination_capacity_bytes)
{
	uint32_t data_length = mws_source_audio_buffer_queued_bytes(buffer);
	uint32_t first_copy_length;
	uint32_t bytes_to_end;

	if (data_length > destination_capacity_bytes) {
		data_length = destination_capacity_bytes;
	}
	data_length -= data_length % sizeof(int16_t);
	first_copy_length = data_length;
	bytes_to_end = buffer->capacity_bytes - buffer->read_offset_bytes;
	if (first_copy_length > bytes_to_end) {
		first_copy_length = bytes_to_end;
	}

	memcpy(
			destination,
			buffer->data + buffer->read_offset_bytes,
			first_copy_length);
	if (first_copy_length < data_length) {
		memcpy(
				destination + first_copy_length,
				buffer->data,
				data_length - first_copy_length);
	}

	return data_length;
}

void mws_source_audio_buffer_consume(
		mws_source_audio_buffer_t *buffer,
		uint32_t data_length)
{
	uint32_t used_bytes = mws_source_audio_buffer_queued_bytes(buffer);

	if (data_length > used_bytes) {
		data_length = used_bytes;
	}
	data_length -= data_length % sizeof(int16_t);
	buffer->read_offset_bytes =
			(buffer->read_offset_bytes + data_length) %
			buffer->capacity_bytes;
	atomic_store_explicit(
			&buffer->used_bytes,
			used_bytes - data_length,
			memory_order_release);
}

uint32_t mws_source_audio_buffer_discard_all(
		mws_source_audio_buffer_t *buffer)
{
	uint32_t discarded_bytes =
			mws_source_audio_buffer_queued_bytes(buffer);

	buffer->read_offset_bytes = 0;
	buffer->write_offset_bytes = 0;
	atomic_store_explicit(&buffer->used_bytes, 0, memory_order_release);

	return discarded_bytes;
}

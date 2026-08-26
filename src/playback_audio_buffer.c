#include "playback_audio_buffer.h"

#include <limits.h>
#include <string.h>

void mws_playback_audio_buffer_initialize(
		mws_playback_audio_buffer_t *buffer,
		uint8_t *storage,
		uint32_t allocation_bytes,
		uint32_t capacity_bytes)
{
	buffer->data = storage;
	buffer->allocation_bytes = allocation_bytes;
	atomic_init(&buffer->capacity_bytes, capacity_bytes);
	atomic_init(&buffer->read_offset_bytes, 0);
	atomic_init(&buffer->write_offset_bytes, 0);
	atomic_init(&buffer->maintenance_requested, 0);
	atomic_init(&buffer->consumer_active, 0);
}

uint32_t mws_playback_audio_buffer_capacity_bytes(
		const mws_playback_audio_buffer_t *buffer)
{
	return atomic_load_explicit(
			&buffer->capacity_bytes,
			memory_order_acquire);
}

uint32_t mws_playback_audio_buffer_queued_bytes(
		const mws_playback_audio_buffer_t *buffer)
{
	uint32_t write_offset = atomic_load_explicit(
			&buffer->write_offset_bytes,
			memory_order_acquire);
	uint32_t read_offset = atomic_load_explicit(
			&buffer->read_offset_bytes,
			memory_order_acquire);

	if (write_offset >= read_offset) {
		return write_offset - read_offset;
	}

	return buffer->allocation_bytes - read_offset + write_offset;
}

bool mws_playback_audio_buffer_begin_read(
		mws_playback_audio_buffer_t *buffer,
		uint32_t *available_bytes)
{
	if (atomic_load_explicit(
			&buffer->maintenance_requested,
			memory_order_seq_cst)) {
		return false;
	}

	atomic_store_explicit(
			&buffer->consumer_active,
			1,
			memory_order_seq_cst);
	if (atomic_load_explicit(
			&buffer->maintenance_requested,
			memory_order_seq_cst)) {
		atomic_store_explicit(
				&buffer->consumer_active,
				0,
				memory_order_seq_cst);
		return false;
	}

	*available_bytes = mws_playback_audio_buffer_queued_bytes(buffer);
	return true;
}

void mws_playback_audio_buffer_end_read(
		mws_playback_audio_buffer_t *buffer)
{
	atomic_store_explicit(
			&buffer->consumer_active,
			0,
			memory_order_seq_cst);
}

void mws_playback_audio_buffer_mix_and_consume(
		mws_playback_audio_buffer_t *buffer,
		int16_t *interleaved_pcm,
		uint32_t sample_count,
		uint32_t channel_count)
{
	uint32_t read_offset = atomic_load_explicit(
			&buffer->read_offset_bytes,
			memory_order_relaxed);
	uint32_t sample_index;

	for (sample_index = 0; sample_index < sample_count; sample_index++) {
		int16_t playback_sample;
		uint32_t channel_index;

		memcpy(
				&playback_sample,
				buffer->data + read_offset,
				sizeof(playback_sample));
		read_offset = (read_offset + sizeof(playback_sample)) %
				buffer->allocation_bytes;

		for (channel_index = 0;
				channel_index < channel_count;
				channel_index++) {
			int32_t mixed_sample =
					interleaved_pcm[
						sample_index * channel_count + channel_index] +
					playback_sample;

			if (mixed_sample > INT16_MAX) {
				mixed_sample = INT16_MAX;
			} else if (mixed_sample < INT16_MIN) {
				mixed_sample = INT16_MIN;
			}
			interleaved_pcm[
					sample_index * channel_count + channel_index] =
					(int16_t) mixed_sample;
		}
	}

	atomic_store_explicit(
			&buffer->read_offset_bytes,
			read_offset,
			memory_order_release);
}

void mws_playback_audio_buffer_request_maintenance(
		mws_playback_audio_buffer_t *buffer)
{
	atomic_store_explicit(
			&buffer->maintenance_requested,
			1,
			memory_order_seq_cst);
}

bool mws_playback_audio_buffer_consumer_is_active(
		const mws_playback_audio_buffer_t *buffer)
{
	return atomic_load_explicit(
			&buffer->consumer_active,
			memory_order_seq_cst) != 0;
}

void mws_playback_audio_buffer_finish_maintenance(
		mws_playback_audio_buffer_t *buffer)
{
	atomic_store_explicit(
			&buffer->maintenance_requested,
			0,
			memory_order_seq_cst);
}

uint32_t mws_playback_audio_buffer_discard_oldest_during_maintenance(
		mws_playback_audio_buffer_t *buffer,
		uint32_t data_length)
{
	uint32_t queued_bytes =
			mws_playback_audio_buffer_queued_bytes(buffer);
	uint32_t read_offset;

	if (data_length > queued_bytes) {
		data_length = queued_bytes;
	}
	data_length -= data_length % sizeof(int16_t);
	read_offset = atomic_load_explicit(
			&buffer->read_offset_bytes,
			memory_order_relaxed);
	read_offset = (read_offset + data_length) % buffer->allocation_bytes;
	atomic_store_explicit(
			&buffer->read_offset_bytes,
			read_offset,
			memory_order_relaxed);

	return data_length;
}

uint32_t mws_playback_audio_buffer_discard_all_during_maintenance(
		mws_playback_audio_buffer_t *buffer)
{
	uint32_t queued_bytes =
			mws_playback_audio_buffer_queued_bytes(buffer);
	uint32_t write_offset = atomic_load_explicit(
			&buffer->write_offset_bytes,
			memory_order_relaxed);

	atomic_store_explicit(
			&buffer->read_offset_bytes,
			write_offset,
			memory_order_relaxed);

	return queued_bytes;
}

uint32_t mws_playback_audio_buffer_reset_during_maintenance(
		mws_playback_audio_buffer_t *buffer,
		uint32_t capacity_bytes)
{
	uint32_t discarded_bytes =
			mws_playback_audio_buffer_queued_bytes(buffer);

	atomic_store_explicit(
			&buffer->read_offset_bytes,
			0,
			memory_order_relaxed);
	atomic_store_explicit(
			&buffer->write_offset_bytes,
			0,
			memory_order_release);
	atomic_store_explicit(
			&buffer->capacity_bytes,
			capacity_bytes,
			memory_order_release);

	return discarded_bytes;
}

void mws_playback_audio_buffer_write(
		mws_playback_audio_buffer_t *buffer,
		const uint8_t *data,
		uint32_t data_length)
{
	uint32_t write_offset = atomic_load_explicit(
			&buffer->write_offset_bytes,
			memory_order_relaxed);
	uint32_t first_copy_length = data_length;
	uint32_t bytes_to_end = buffer->allocation_bytes - write_offset;

	if (first_copy_length > bytes_to_end) {
		first_copy_length = bytes_to_end;
	}

	memcpy(buffer->data + write_offset, data, first_copy_length);
	if (first_copy_length < data_length) {
		memcpy(
				buffer->data,
				data + first_copy_length,
				data_length - first_copy_length);
	}

	write_offset = (write_offset + data_length) % buffer->allocation_bytes;
	atomic_store_explicit(
			&buffer->write_offset_bytes,
			write_offset,
			memory_order_release);
}

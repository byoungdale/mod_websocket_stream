#include "captured_audio_buffer.h"

#include <string.h>

static switch_size_t calculate_pcm_capacity_bytes(
		uint32_t sample_rate,
		uint32_t channels,
		uint32_t duration_ms)
{
	uint64_t sample_count;
	uint64_t capacity_bytes;

	if (!sample_rate || !channels || !duration_ms) {
		return 0;
	}

	sample_count = (uint64_t) sample_rate * duration_ms / 1000;
	if (sample_count > UINT64_MAX / channels / sizeof(int16_t)) {
		return 0;
	}
	capacity_bytes = sample_count * channels * sizeof(int16_t);
	if (capacity_bytes > SIZE_MAX) {
		return 0;
	}

	return (switch_size_t) capacity_bytes;
}

static void record_drop(
		mws_captured_audio_buffer_t *buffer,
		switch_size_t dropped_bytes)
{
	if (!dropped_bytes) {
		return;
	}

	atomic_fetch_add_explicit(
			&buffer->drop_event_count,
			1,
			memory_order_relaxed);
	atomic_fetch_add_explicit(
			&buffer->dropped_byte_count,
			dropped_bytes,
			memory_order_relaxed);
}

static void discard_oldest_ring_bytes(
		mws_captured_audio_buffer_t *buffer,
		switch_size_t data_length)
{
	if (data_length > buffer->ring_used_bytes) {
		data_length = buffer->ring_used_bytes;
	}

	buffer->ring_read_offset_bytes =
			(buffer->ring_read_offset_bytes + data_length) %
			buffer->ring_capacity_bytes;
	buffer->ring_used_bytes -= data_length;
}

static void write_ring(
		mws_captured_audio_buffer_t *buffer,
		const uint8_t *data,
		switch_size_t data_length)
{
	switch_size_t first_copy_length = data_length;
	switch_size_t bytes_to_end = buffer->ring_capacity_bytes -
			buffer->ring_write_offset_bytes;

	if (first_copy_length > bytes_to_end) {
		first_copy_length = bytes_to_end;
	}

	memcpy(
			buffer->ring_storage + buffer->ring_write_offset_bytes,
			data,
			first_copy_length);

	if (first_copy_length < data_length) {
		memcpy(
				buffer->ring_storage,
				data + first_copy_length,
				data_length - first_copy_length);
	}

	buffer->ring_write_offset_bytes =
			(buffer->ring_write_offset_bytes + data_length) %
			buffer->ring_capacity_bytes;
	buffer->ring_used_bytes += data_length;
}

static void store_fallback_chunk(
		mws_captured_audio_buffer_t *buffer,
		const uint8_t *data,
		switch_size_t data_length,
		uint32_t sample_rate,
		uint32_t channels)
{
	const uint8_t *write_data = data;
	switch_size_t write_length = data_length;
	switch_size_t chunk_capacity_bytes = calculate_pcm_capacity_bytes(
			sample_rate,
			channels,
			buffer->chunk_duration_ms);
	switch_size_t dropped_bytes = 0;

	if (!chunk_capacity_bytes || chunk_capacity_bytes >
			buffer->fallback_capacity_bytes) {
		record_drop(buffer, data_length);
		atomic_fetch_add_explicit(
				&buffer->fallback_drop_count,
				1,
				memory_order_relaxed);
		return;
	}

	if (switch_mutex_trylock(buffer->fallback_mutex) !=
			SWITCH_STATUS_SUCCESS) {
		record_drop(buffer, data_length);
		atomic_fetch_add_explicit(
				&buffer->fallback_drop_count,
				1,
				memory_order_relaxed);
		return;
	}

	if (write_length > chunk_capacity_bytes) {
		dropped_bytes += write_length - chunk_capacity_bytes;
		write_data += write_length - chunk_capacity_bytes;
		write_length = chunk_capacity_bytes;
	}

	if (switch_atomic_read(&buffer->fallback_ready)) {
		dropped_bytes += buffer->fallback_length_bytes;
		atomic_fetch_add_explicit(
				&buffer->fallback_overwrite_count,
				1,
				memory_order_relaxed);
		atomic_fetch_add_explicit(
				&buffer->fallback_overwritten_byte_count,
				buffer->fallback_length_bytes,
				memory_order_relaxed);
	}

	memcpy(buffer->fallback_storage, write_data, write_length);
	buffer->fallback_length_bytes = write_length;
	buffer->fallback_sample_rate = sample_rate;
	buffer->fallback_channels = channels;
	atomic_fetch_add_explicit(
			&buffer->fallback_store_count,
			1,
			memory_order_relaxed);
	switch_atomic_set(&buffer->fallback_ready, 1);

	record_drop(buffer, dropped_bytes);
	switch_mutex_unlock(buffer->fallback_mutex);
}

static switch_size_t take_fallback_chunk(
		mws_captured_audio_buffer_t *buffer,
		uint8_t *data,
		switch_size_t data_capacity,
		uint64_t *format_generation,
		uint32_t *sample_rate,
		uint32_t *channels)
{
	switch_size_t data_length;
	switch_size_t ring_capacity_bytes;
	switch_size_t stale_bytes;

	if (switch_mutex_trylock(buffer->fallback_mutex) !=
			SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	if (!switch_atomic_read(&buffer->fallback_ready)) {
		switch_mutex_unlock(buffer->fallback_mutex);
		return 0;
	}

	if (switch_mutex_trylock(buffer->ring_mutex) != SWITCH_STATUS_SUCCESS) {
		switch_mutex_unlock(buffer->fallback_mutex);
		return 0;
	}

	data_length = buffer->fallback_length_bytes;
	ring_capacity_bytes = calculate_pcm_capacity_bytes(
			buffer->fallback_sample_rate,
			buffer->fallback_channels,
			buffer->ring_duration_ms);

	if (!data_length || data_length > data_capacity ||
			!ring_capacity_bytes || ring_capacity_bytes >
			buffer->ring_allocation_bytes) {
		atomic_fetch_add_explicit(
				&buffer->fallback_abandoned_byte_count,
				data_length,
				memory_order_relaxed);
		buffer->fallback_length_bytes = 0;
		switch_atomic_set(&buffer->fallback_ready, 0);
		switch_mutex_unlock(buffer->ring_mutex);
		switch_mutex_unlock(buffer->fallback_mutex);
		return 0;
	}

	stale_bytes = buffer->ring_used_bytes;
	buffer->ring_capacity_bytes = ring_capacity_bytes;
	buffer->ring_read_offset_bytes = 0;
	buffer->ring_write_offset_bytes = 0;
	buffer->ring_used_bytes = 0;

	if (buffer->fallback_sample_rate != buffer->sample_rate ||
			buffer->fallback_channels != buffer->channels) {
		buffer->sample_rate = buffer->fallback_sample_rate;
		buffer->channels = buffer->fallback_channels;
		buffer->format_generation++;
	}

	memcpy(data, buffer->fallback_storage, data_length);
	*format_generation = buffer->format_generation;
	*sample_rate = buffer->sample_rate;
	*channels = buffer->channels;
	buffer->fallback_length_bytes = 0;
	atomic_fetch_add_explicit(
			&buffer->stale_ring_discarded_byte_count,
			stale_bytes,
			memory_order_relaxed);
	switch_atomic_set(&buffer->fallback_ready, 0);

	switch_mutex_unlock(buffer->ring_mutex);
	switch_mutex_unlock(buffer->fallback_mutex);

	return data_length;
}

switch_status_t mws_captured_audio_buffer_initialize(
		mws_captured_audio_buffer_t *buffer,
		uint8_t *ring_storage,
		switch_size_t ring_allocation_bytes,
		switch_size_t initial_ring_capacity_bytes,
		uint8_t *fallback_storage,
		switch_size_t fallback_capacity_bytes,
		uint32_t initial_sample_rate,
		uint32_t initial_channels,
		uint32_t ring_duration_ms,
		uint32_t chunk_duration_ms,
		switch_memory_pool_t *pool)
{
	if (!buffer || !ring_storage || !ring_allocation_bytes ||
			!initial_ring_capacity_bytes ||
			initial_ring_capacity_bytes > ring_allocation_bytes ||
			!fallback_storage || !fallback_capacity_bytes ||
			!initial_sample_rate || !initial_channels ||
			!ring_duration_ms || !chunk_duration_ms || !pool) {
		return SWITCH_STATUS_FALSE;
	}

	memset(buffer, 0, sizeof(*buffer));
	buffer->ring_storage = ring_storage;
	buffer->ring_allocation_bytes = ring_allocation_bytes;
	buffer->ring_capacity_bytes = initial_ring_capacity_bytes;
	buffer->sample_rate = initial_sample_rate;
	buffer->channels = initial_channels;
	buffer->format_generation = 1;
	buffer->fallback_storage = fallback_storage;
	buffer->fallback_capacity_bytes = fallback_capacity_bytes;
	buffer->ring_duration_ms = ring_duration_ms;
	buffer->chunk_duration_ms = chunk_duration_ms;
	switch_atomic_set(&buffer->fallback_ready, 0);
	atomic_init(&buffer->drop_event_count, 0);
	atomic_init(&buffer->dropped_byte_count, 0);
	atomic_init(&buffer->invalid_frame_count, 0);
	atomic_init(&buffer->ring_lock_contention_count, 0);
	atomic_init(&buffer->fallback_drop_count, 0);
	atomic_init(&buffer->fallback_store_count, 0);
	atomic_init(&buffer->fallback_overwrite_count, 0);
	atomic_init(&buffer->fallback_overwritten_byte_count, 0);
	atomic_init(&buffer->fallback_abandoned_byte_count, 0);
	atomic_init(&buffer->stale_ring_discarded_byte_count, 0);

	if (switch_mutex_init(
				&buffer->ring_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_mutex_init(
				&buffer->fallback_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

void mws_captured_audio_buffer_write(
		mws_captured_audio_buffer_t *buffer,
		const void *data,
		switch_size_t data_length,
		uint32_t sample_rate,
		uint32_t channels)
{
	const uint8_t *write_data = (const uint8_t *) data;
	switch_size_t write_length = data_length;
	switch_size_t ring_capacity_bytes;
	switch_size_t pcm_frame_size;
	switch_size_t dropped_bytes = 0;

	if (!buffer || !buffer->ring_storage || !data || !data_length) {
		return;
	}

	ring_capacity_bytes = calculate_pcm_capacity_bytes(
			sample_rate,
			channels,
			buffer->ring_duration_ms);
	if (!ring_capacity_bytes || ring_capacity_bytes >
			buffer->ring_allocation_bytes) {
		mws_captured_audio_buffer_record_invalid_frame(buffer);
		record_drop(buffer, data_length);
		return;
	}

	pcm_frame_size = channels * sizeof(int16_t);
	if (!pcm_frame_size || data_length % pcm_frame_size) {
		mws_captured_audio_buffer_record_invalid_frame(buffer);
		record_drop(buffer, data_length);
		return;
	}

	if (switch_atomic_read(&buffer->fallback_ready)) {
		store_fallback_chunk(
				buffer,
				write_data,
				write_length,
				sample_rate,
				channels);
		return;
	}

	if (switch_mutex_trylock(buffer->ring_mutex) != SWITCH_STATUS_SUCCESS) {
		atomic_fetch_add_explicit(
				&buffer->ring_lock_contention_count,
				1,
				memory_order_relaxed);
		store_fallback_chunk(
				buffer,
				write_data,
				write_length,
				sample_rate,
				channels);
		return;
	}

	if (sample_rate != buffer->sample_rate || channels != buffer->channels) {
		dropped_bytes = buffer->ring_used_bytes;
		buffer->ring_capacity_bytes = ring_capacity_bytes;
		buffer->ring_read_offset_bytes = 0;
		buffer->ring_write_offset_bytes = 0;
		buffer->ring_used_bytes = 0;
		buffer->sample_rate = sample_rate;
		buffer->channels = channels;
		buffer->format_generation++;
	}

	if (write_length > buffer->ring_capacity_bytes) {
		dropped_bytes += buffer->ring_used_bytes + write_length -
				buffer->ring_capacity_bytes;
		write_data += write_length - buffer->ring_capacity_bytes;
		write_length = buffer->ring_capacity_bytes;
		buffer->ring_read_offset_bytes = 0;
		buffer->ring_write_offset_bytes = 0;
		buffer->ring_used_bytes = 0;
	}

	if (buffer->ring_capacity_bytes - buffer->ring_used_bytes < write_length) {
		switch_size_t bytes_to_drop = write_length -
				(buffer->ring_capacity_bytes - buffer->ring_used_bytes);

		discard_oldest_ring_bytes(buffer, bytes_to_drop);
		dropped_bytes += bytes_to_drop;
	}

	write_ring(buffer, write_data, write_length);
	record_drop(buffer, dropped_bytes);
	switch_mutex_unlock(buffer->ring_mutex);
}

switch_size_t mws_captured_audio_buffer_read(
		mws_captured_audio_buffer_t *buffer,
		uint8_t *data,
		switch_size_t data_capacity,
		uint64_t *format_generation,
		uint32_t *sample_rate,
		uint32_t *channels)
{
	switch_size_t data_length;
	switch_size_t first_copy_length;
	switch_size_t bytes_to_end;
	switch_size_t chunk_capacity_bytes;
	switch_size_t pcm_frame_size;
	switch_size_t fallback_chunk_length;

	if (!buffer || !data || !data_capacity || !format_generation ||
			!sample_rate || !channels) {
		return 0;
	}

	if (switch_atomic_read(&buffer->fallback_ready)) {
		fallback_chunk_length = take_fallback_chunk(
				buffer,
				data,
				data_capacity,
				format_generation,
				sample_rate,
				channels);
		if (fallback_chunk_length ||
				switch_atomic_read(&buffer->fallback_ready)) {
			return fallback_chunk_length;
		}
	}

	if (switch_mutex_trylock(buffer->ring_mutex) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	pcm_frame_size = buffer->channels * sizeof(int16_t);
	chunk_capacity_bytes = calculate_pcm_capacity_bytes(
			buffer->sample_rate,
			buffer->channels,
			buffer->chunk_duration_ms);
	data_length = buffer->ring_used_bytes;
	if (data_length > data_capacity) {
		data_length = data_capacity;
	}
	if (data_length > chunk_capacity_bytes) {
		data_length = chunk_capacity_bytes;
	}
	data_length -= data_length % pcm_frame_size;

	if (!data_length) {
		switch_mutex_unlock(buffer->ring_mutex);
		return 0;
	}

	first_copy_length = data_length;
	bytes_to_end = buffer->ring_capacity_bytes -
			buffer->ring_read_offset_bytes;
	if (first_copy_length > bytes_to_end) {
		first_copy_length = bytes_to_end;
	}

	memcpy(
			data,
			buffer->ring_storage + buffer->ring_read_offset_bytes,
			first_copy_length);

	if (first_copy_length < data_length) {
		memcpy(
				data + first_copy_length,
				buffer->ring_storage,
				data_length - first_copy_length);
	}

	discard_oldest_ring_bytes(buffer, data_length);
	*format_generation = buffer->format_generation;
	*sample_rate = buffer->sample_rate;
	*channels = buffer->channels;
	switch_mutex_unlock(buffer->ring_mutex);

	return data_length;
}

switch_bool_t mws_captured_audio_buffer_has_data(
		mws_captured_audio_buffer_t *buffer)
{
	switch_bool_t available;

	if (!buffer) {
		return SWITCH_FALSE;
	}
	if (switch_atomic_read(&buffer->fallback_ready)) {
		return SWITCH_TRUE;
	}
	if (switch_mutex_trylock(buffer->ring_mutex) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}

	available = buffer->ring_used_bytes ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(buffer->ring_mutex);

	return available;
}

switch_size_t mws_captured_audio_buffer_discard_ring(
		mws_captured_audio_buffer_t *buffer)
{
	switch_size_t discarded_bytes;

	if (!buffer || !buffer->ring_mutex) {
		return 0;
	}

	switch_mutex_lock(buffer->ring_mutex);
	discarded_bytes = buffer->ring_used_bytes;
	buffer->ring_read_offset_bytes = 0;
	buffer->ring_write_offset_bytes = 0;
	buffer->ring_used_bytes = 0;
	switch_mutex_unlock(buffer->ring_mutex);
	record_drop(buffer, discarded_bytes);

	return discarded_bytes;
}

void mws_captured_audio_buffer_discard_fallback(
		mws_captured_audio_buffer_t *buffer)
{
	if (!buffer || !buffer->fallback_mutex) {
		return;
	}

	switch_mutex_lock(buffer->fallback_mutex);
	if (switch_atomic_read(&buffer->fallback_ready)) {
		atomic_fetch_add_explicit(
				&buffer->fallback_abandoned_byte_count,
				buffer->fallback_length_bytes,
				memory_order_relaxed);
	}
	buffer->fallback_length_bytes = 0;
	switch_atomic_set(&buffer->fallback_ready, 0);
	switch_mutex_unlock(buffer->fallback_mutex);
}

void mws_captured_audio_buffer_record_invalid_frame(
		mws_captured_audio_buffer_t *buffer)
{
	if (buffer) {
		atomic_fetch_add_explicit(
				&buffer->invalid_frame_count,
				1,
				memory_order_relaxed);
	}
}

void mws_captured_audio_buffer_snapshot(
		mws_captured_audio_buffer_t *buffer,
		mws_captured_audio_buffer_statistics_t *statistics)
{
	if (!buffer || !statistics) {
		return;
	}

	memset(statistics, 0, sizeof(*statistics));
	switch_mutex_lock(buffer->ring_mutex);
	statistics->queued_bytes = buffer->ring_used_bytes;
	statistics->capacity_bytes = buffer->ring_capacity_bytes;
	statistics->sample_rate = buffer->sample_rate;
	statistics->channels = buffer->channels;
	statistics->format_generation = buffer->format_generation;
	switch_mutex_unlock(buffer->ring_mutex);

	switch_mutex_lock(buffer->fallback_mutex);
	if (switch_atomic_read(&buffer->fallback_ready)) {
		statistics->fallback_queued_bytes = buffer->fallback_length_bytes;
	}
	switch_mutex_unlock(buffer->fallback_mutex);

	statistics->drop_events = atomic_load_explicit(
			&buffer->drop_event_count,
			memory_order_relaxed);
	statistics->dropped_bytes = atomic_load_explicit(
			&buffer->dropped_byte_count,
			memory_order_relaxed);
	statistics->invalid_frames = atomic_load_explicit(
			&buffer->invalid_frame_count,
			memory_order_relaxed);
	statistics->ring_lock_contentions = atomic_load_explicit(
			&buffer->ring_lock_contention_count,
			memory_order_relaxed);
	statistics->fallback_drops = atomic_load_explicit(
			&buffer->fallback_drop_count,
			memory_order_relaxed);
	statistics->fallback_stores = atomic_load_explicit(
			&buffer->fallback_store_count,
			memory_order_relaxed);
	statistics->fallback_overwrites = atomic_load_explicit(
			&buffer->fallback_overwrite_count,
			memory_order_relaxed);
	statistics->fallback_overwritten_bytes = atomic_load_explicit(
			&buffer->fallback_overwritten_byte_count,
			memory_order_relaxed);
	statistics->fallback_abandoned_bytes = atomic_load_explicit(
			&buffer->fallback_abandoned_byte_count,
			memory_order_relaxed);
	statistics->stale_ring_discarded_bytes = atomic_load_explicit(
			&buffer->stale_ring_discarded_byte_count,
			memory_order_relaxed);
}

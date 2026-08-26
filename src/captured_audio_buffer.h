#ifndef MWS_CAPTURED_AUDIO_BUFFER_H
#define MWS_CAPTURED_AUDIO_BUFFER_H

#include <switch.h>

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
	uint8_t *ring_storage;
	switch_mutex_t *ring_mutex;
	switch_size_t ring_allocation_bytes;
	switch_size_t ring_capacity_bytes;
	switch_size_t ring_read_offset_bytes;
	switch_size_t ring_write_offset_bytes;
	switch_size_t ring_used_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint64_t format_generation;
	uint8_t *fallback_storage;
	switch_mutex_t *fallback_mutex;
	switch_size_t fallback_capacity_bytes;
	switch_size_t fallback_length_bytes;
	uint32_t fallback_sample_rate;
	uint32_t fallback_channels;
	switch_atomic_t fallback_ready;
	uint32_t ring_duration_ms;
	uint32_t chunk_duration_ms;
	_Atomic uint64_t drop_event_count;
	_Atomic uint64_t dropped_byte_count;
	_Atomic uint64_t invalid_frame_count;
	_Atomic uint64_t ring_lock_contention_count;
	_Atomic uint64_t fallback_drop_count;
	_Atomic uint64_t fallback_store_count;
	_Atomic uint64_t fallback_overwrite_count;
	_Atomic uint64_t fallback_overwritten_byte_count;
	_Atomic uint64_t fallback_abandoned_byte_count;
	_Atomic uint64_t stale_ring_discarded_byte_count;
} mws_captured_audio_buffer_t;

typedef struct {
	switch_size_t queued_bytes;
	switch_size_t capacity_bytes;
	switch_size_t fallback_queued_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint64_t format_generation;
	uint64_t drop_events;
	uint64_t dropped_bytes;
	uint64_t invalid_frames;
	uint64_t ring_lock_contentions;
	uint64_t fallback_drops;
	uint64_t fallback_stores;
	uint64_t fallback_overwrites;
	uint64_t fallback_overwritten_bytes;
	uint64_t fallback_abandoned_bytes;
	uint64_t stale_ring_discarded_bytes;
} mws_captured_audio_buffer_statistics_t;

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
		switch_memory_pool_t *pool);

void mws_captured_audio_buffer_write(
		mws_captured_audio_buffer_t *buffer,
		const void *data,
		switch_size_t data_length,
		uint32_t sample_rate,
		uint32_t channels);

switch_size_t mws_captured_audio_buffer_read(
		mws_captured_audio_buffer_t *buffer,
		uint8_t *data,
		switch_size_t data_capacity,
		uint64_t *format_generation,
		uint32_t *sample_rate,
		uint32_t *channels);

switch_bool_t mws_captured_audio_buffer_has_data(
		mws_captured_audio_buffer_t *buffer);

switch_size_t mws_captured_audio_buffer_discard_ring(
		mws_captured_audio_buffer_t *buffer);

void mws_captured_audio_buffer_discard_fallback(
		mws_captured_audio_buffer_t *buffer);

void mws_captured_audio_buffer_record_invalid_frame(
		mws_captured_audio_buffer_t *buffer);

void mws_captured_audio_buffer_snapshot(
		mws_captured_audio_buffer_t *buffer,
		mws_captured_audio_buffer_statistics_t *statistics);

#endif

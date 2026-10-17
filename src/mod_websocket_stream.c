#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <switch.h>
#include <stdatomic.h>

#include "bounded_stream.h"
#include "captured_audio_buffer.h"
#include "captured_audio_resampler.h"
#include "freeswitch_command.h"
#include "http_headers.h"
#include "playback_audio_buffer.h"
#include "playback_audio_resampler.h"
#include "playback_timing.h"
#include "resource_limits.h"
#include "source_audio_buffer.h"
#include "websocket_audio_input.h"
#include "websocket_protocol.h"

#define MWS_MINIMUM_CURL_VERSION CURL_VERSION_BITS(8, 21, 0)
#define MWS_API_SYNTAX "status [json]|reload"
#define MWS_UUID_API_SYNTAX \
	"<channel-uuid> clear [stream-id]|" \
	"<channel-uuid> send [stream-id|all] <json>|" \
	"<channel-uuid> status [stream-id] [json]"
#define MWS_APP_SYNTAX "<ws-url>"
#define MWS_STOP_APP_SYNTAX "[stream-id|all]"
#define MWS_CHANNEL_HEADER_VARIABLE_PREFIX "websocket_stream_header_"
#define MWS_TRANSPORT_EVENT_SUBCLASS "mod_websocket_stream::transport"
#define MWS_STATISTICS_EVENT_SUBCLASS "mod_websocket_stream::statistics"
#define MWS_ERROR_EVENT_SUBCLASS "mod_websocket_stream::error"
#define MWS_MESSAGE_EVENT_SUBCLASS "mod_websocket_stream::message"
#define MWS_API_COMMANDS_VARIABLE "websocket_stream_api_commands"
#define MWS_MAXIMUM_MESSAGE_EVENT_NAME_BYTES 128
#define MWS_PLAYBACK_BUFFER_OVERFLOW_ERROR "playback_buffer_overflow"
#define MWS_TRANSPORT_STATE_VARIABLE "websocket_stream_last_transport_state"
#define MWS_CURL_CODE_VARIABLE "websocket_stream_last_curl_code"
#define MWS_CURL_ERROR_VARIABLE "websocket_stream_last_curl_error"
#define MWS_LAST_STREAM_ID_VARIABLE "websocket_stream_last_stream_id"
#define MWS_LAST_STARTED_STREAM_ID_VARIABLE "websocket_stream_last_started_id"
#define MWS_LAST_TARGET_RATE_HZ_VARIABLE \
	"websocket_stream_last_target_rate_hz"
#define MWS_LAST_CAPTURED_DROP_EVENTS_VARIABLE \
	"websocket_stream_last_captured_drop_events"
#define MWS_LAST_CAPTURED_DROPPED_BYTES_VARIABLE \
	"websocket_stream_last_captured_dropped_bytes"
#define MWS_LAST_PLAYBACK_RECEIVED_SAMPLES_VARIABLE \
	"websocket_stream_last_playback_received_samples"
#define MWS_LAST_PLAYBACK_SOURCE_DROPPED_SAMPLES_VARIABLE \
	"websocket_stream_last_playback_source_dropped_samples"
#define MWS_LAST_PLAYBACK_SOURCE_CAPACITY_BYTES_VARIABLE \
	"websocket_stream_last_playback_source_capacity_bytes"
#define MWS_LAST_PLAYBACK_BUFFERED_SAMPLES_VARIABLE \
	"websocket_stream_last_playback_buffered_samples"
#define MWS_LAST_PLAYBACK_MIXED_SAMPLES_VARIABLE \
	"websocket_stream_last_playback_mixed_samples"
#define MWS_LAST_PLAYBACK_DROPPED_SAMPLES_VARIABLE \
	"websocket_stream_last_playback_dropped_samples"
#define MWS_LAST_PLAYBACK_PEAK_QUEUED_BYTES_VARIABLE \
	"websocket_stream_last_playback_peak_queued_bytes"
#define MWS_LAST_PLAYBACK_CAPACITY_BYTES_VARIABLE \
	"websocket_stream_last_playback_capacity_bytes"
#define MWS_LAST_PLAYBACK_UNDERFLOW_FRAMES_VARIABLE \
	"websocket_stream_last_playback_underflow_frames"
#define MWS_LAST_PLAYBACK_INVALID_FRAMES_VARIABLE \
	"websocket_stream_last_playback_invalid_frames"
#define MWS_LAST_PLAYBACK_RATE_ADJUSTMENT_PPM_VARIABLE \
	"websocket_stream_last_playback_rate_adjustment_ppm"
#define MWS_LAST_PLAYBACK_RATE_MAXIMUM_ADJUSTMENT_PPM_VARIABLE \
	"websocket_stream_last_playback_rate_maximum_adjustment_ppm"
#define MWS_LAST_PLAYBACK_RATE_FILTERED_QUEUE_US_VARIABLE \
	"websocket_stream_last_playback_rate_filtered_queue_us"
#define MWS_LAST_PLAYBACK_RATE_TARGET_QUEUE_US_VARIABLE \
	"websocket_stream_last_playback_rate_target_queue_us"
#define MWS_LAST_PLAYBACK_RATE_UPDATES_VARIABLE \
	"websocket_stream_last_playback_rate_updates"
#define MWS_LAST_PLAYBACK_RATE_LIMIT_DURATION_US_VARIABLE \
	"websocket_stream_last_playback_rate_limit_duration_us"
#define MWS_LAST_PLAYBACK_CALLBACK_MAX_LATENESS_US_VARIABLE \
	"websocket_stream_last_playback_callback_max_lateness_us"
#define MWS_LAST_PLAYBACK_CALLBACK_GAP_INTERVALS_VARIABLE \
	"websocket_stream_last_playback_callback_gap_intervals"
#define MWS_LAST_PLAYBACK_MODULE_DISPATCH_MAX_DELAY_US_VARIABLE \
	"websocket_stream_last_playback_module_dispatch_max_delay_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_WITH_SOURCE_AUDIO_COUNT_VARIABLE \
	"websocket_stream_last_playback_underflow_with_source_audio_count"
#define MWS_LAST_PLAYBACK_MAX_GAP_AGE_US_VARIABLE \
	"websocket_stream_last_playback_max_gap_age_us"
#define MWS_LAST_PLAYBACK_MAX_GAP_LATENESS_US_VARIABLE \
	"websocket_stream_last_playback_max_gap_lateness_us"
#define MWS_LAST_PLAYBACK_MAX_GAP_DISPATCH_DELAY_US_VARIABLE \
	"websocket_stream_last_playback_max_gap_dispatch_delay_us"
#define MWS_LAST_PLAYBACK_MAX_GAP_SOURCE_QUEUED_BYTES_VARIABLE \
	"websocket_stream_last_playback_max_gap_source_queued_bytes"
#define MWS_LAST_PLAYBACK_MAX_GAP_QUEUED_BYTES_VARIABLE \
	"websocket_stream_last_playback_max_gap_queued_bytes"
#define MWS_LAST_PLAYBACK_UNDERFLOW_AGE_US_VARIABLE \
	"websocket_stream_last_playback_underflow_age_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_CALLBACK_LATENESS_US_VARIABLE \
	"websocket_stream_last_playback_underflow_callback_lateness_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_DISPATCH_DELAY_US_VARIABLE \
	"websocket_stream_last_playback_underflow_dispatch_delay_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_SOURCE_QUEUED_BYTES_VARIABLE \
	"websocket_stream_last_playback_underflow_source_queued_bytes"
#define MWS_LAST_PLAYBACK_UNDERFLOW_QUEUED_BYTES_VARIABLE \
	"websocket_stream_last_playback_underflow_queued_bytes"
#define MWS_LAST_PLAYBACK_UNDERFLOW_WEBSOCKET_AUDIO_AGE_US_VARIABLE \
	"websocket_stream_last_playback_underflow_websocket_audio_age_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_BUFFER_WRITE_AGE_US_VARIABLE \
	"websocket_stream_last_playback_underflow_buffer_write_age_us"
#define MWS_LAST_PLAYBACK_UNDERFLOW_WORKER_VISIT_AGE_US_VARIABLE \
	"websocket_stream_last_playback_underflow_worker_visit_age_us"
#define MWS_DEFAULT_SAMPLE_RATE 16000
#define MWS_MINIMUM_SAMPLE_RATE 8000
#define MWS_MAXIMUM_SAMPLE_RATE 48000
#define MWS_NATIVE_RING_DURATION_MS 500
#define MWS_NATIVE_CHUNK_DURATION_MS 20
#define MWS_PLAYBACK_BUFFER_DURATION_MS 500
#define MWS_PLAYBACK_CHUNK_DURATION_MS 20
#define MWS_PLAYBACK_SOURCE_RING_DURATION_MS 2000
#define MWS_PLAYBACK_START_BUFFER_MS 60
#define MWS_PLAYBACK_RATE_UPDATE_INTERVAL_US 200000
#define MWS_MAX_MEDIA_FRAMES_PER_CALLBACK 8
#define MWS_MAXIMUM_OUTBOUND_CHANNELS 2
#define MWS_MAXIMUM_PLAYBACK_CHANNELS 2
/* Two regular commands and one remove per stream, plus module stop. */
#define MWS_MAXIMUM_QUEUED_REGULAR_WORKER_COMMANDS \
	(MWS_HARD_MAXIMUM_STREAMS * 2U)
#define MWS_WORKER_COMMAND_QUEUE_CAPACITY \
	(MWS_MAXIMUM_QUEUED_REGULAR_WORKER_COMMANDS + \
	 MWS_HARD_MAXIMUM_STREAMS + 1U)
#define MWS_WORKER_POLL_INTERVAL_US 10000
#define MWS_WORKER_ACK_POLL_INTERVAL_US 100000
#define MWS_AUTOMATIC_MAXIMUM_TRANSPORT_WORKERS 8U
#define MWS_HARD_MAXIMUM_TRANSPORT_WORKERS 32U
#define MWS_CONNECT_TIMEOUT_MS 5000L
#define MWS_CLOSE_HANDSHAKE_TIMEOUT_US 500000
#define MWS_DEFAULT_RECONNECT_ATTEMPTS 5
#define MWS_MAXIMUM_RECONNECT_ATTEMPTS 100
#define MWS_DEFAULT_RECONNECT_DELAY_MS 1000
#define MWS_MINIMUM_RECONNECT_DELAY_MS 100
#define MWS_MAXIMUM_RECONNECT_DELAY_MS 30000
#define MWS_USER_AGENT "mod_websocket_stream/0.0.1"
#ifndef MWS_BUILD_ID
#define MWS_BUILD_ID "unknown"
#endif
#define MWS_CHANNEL_CONTEXT_PRIVATE \
	"mod_websocket_stream::channel_context:" MWS_BUILD_ID
#define MWS_MAXIMUM_TEXT_MESSAGE_BYTES 4096
#define MWS_TEXT_RESPONSE_QUEUE_CAPACITY 16
#define MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY 8
#define MWS_MAXIMUM_INTERNAL_RESPONSE_BYTES 1024
#define MWS_API_COMMAND_QUEUE_CAPACITY 1024
#define MWS_API_COMMAND_QUEUE_STORAGE_CAPACITY \
	(MWS_API_COMMAND_QUEUE_CAPACITY + 1U)
#define MWS_CLEANUP_QUEUE_CAPACITY 1024
#define MWS_CLEANUP_QUEUE_STORAGE_CAPACITY \
	(MWS_CLEANUP_QUEUE_CAPACITY + 2U)
#define MWS_API_RESPONSE_ALLOCATION_ERROR \
	"-ERR could not allocate FreeSWITCH API response\n"
#define MWS_DEFAULT_STATISTICS_INTERVAL_MS 10000
#define MWS_MAXIMUM_STATISTICS_INTERVAL_MS 3600000

SWITCH_STANDARD_APP(websocket_stream_app);
SWITCH_STANDARD_APP(websocket_stream_stop_app);
SWITCH_STANDARD_API(websocket_stream_api);
SWITCH_STANDARD_API(uuid_websocket_stream_api);
SWITCH_MODULE_LOAD_FUNCTION(mod_websocket_stream_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_websocket_stream_shutdown);

typedef struct mws_stream_s mws_stream_t;
typedef struct mws_channel_s mws_channel_t;
typedef struct mws_transport_worker_s mws_transport_worker_t;

static switch_status_t stop_transport_workers(void);
static switch_status_t stop_transport_worker(mws_transport_worker_t *worker);
static void stop_transport_worker_for_lifetime_safety(
		mws_transport_worker_t *worker,
		switch_bool_t cleanup_streams);
static switch_status_t stop_api_command_worker(void);
static switch_status_t stop_cleanup_worker(void);
static uint32_t calculate_reconnect_delay_ms(mws_stream_t *stream_context);
static switch_bool_t websocket_stream_media_bug_callback(
		switch_media_bug_t *bug,
		void *user_data,
		switch_abc_type_t type);
static switch_status_t stop_registered_stream(
		switch_core_session_t *session,
		mws_stream_t *stream_context);
static switch_status_t stop_registered_streams(
		switch_core_session_t *session,
		mws_stream_t **stream_contexts,
		switch_size_t stream_count,
		switch_size_t *stopped_count);
static void finish_detached_stream_cleanup(mws_stream_t *stream_context);
static void defer_claimed_stream_cleanup(mws_stream_t *stream_context);
static void defer_claimed_stream_cleanup_with_borrow(
		mws_stream_t *stream_context);

SWITCH_MODULE_DEFINITION(
    mod_websocket_stream,
    mod_websocket_stream_load,
    mod_websocket_stream_shutdown,
    NULL);

typedef enum {
	MWS_CHANNEL_TO_WEBSOCKET_READ = 1,
	MWS_CHANNEL_TO_WEBSOCKET_WRITE,
	MWS_CHANNEL_TO_WEBSOCKET_MIXED,
	MWS_CHANNEL_TO_WEBSOCKET_STEREO
} mws_channel_to_websocket_t;

typedef enum {
	MWS_WEBSOCKET_TO_CHANNEL_WRITE = 1,
	MWS_WEBSOCKET_TO_CHANNEL_READ
} mws_websocket_to_channel_t;

static switch_bool_t stream_is_registered(mws_stream_t *stream_context);
static switch_bool_t claim_stream_teardown(mws_stream_t *stream_context);
static switch_bool_t complete_stream_startup(mws_stream_t *stream_context);
static void wait_for_stream_startup(mws_stream_t *stream_context);

typedef struct mws_stream_request_header_s {
	const char *name;
	const char *value;
	struct mws_stream_request_header_s *next;
} mws_stream_request_header_t;

typedef enum {
	MWS_WORKER_COMMAND_ADD = 1,
	MWS_WORKER_COMMAND_REMOVE,
	MWS_WORKER_COMMAND_CLEAR,
	MWS_WORKER_COMMAND_STOP
} mws_worker_command_type_t;

typedef enum {
	MWS_TRANSPORT_NEW = 0,
	MWS_TRANSPORT_CONNECTING,
	MWS_TRANSPORT_OPEN,
	MWS_TRANSPORT_RECONNECTING,
	MWS_TRANSPORT_CLOSED,
	MWS_TRANSPORT_FAILED
} mws_transport_state_t;

typedef enum {
	MWS_PLAYBACK_BUFFERING = 0,
	MWS_PLAYBACK_PLAYING
} mws_playback_state_t;

typedef struct {
	mws_worker_command_type_t type;
	mws_stream_t *stream_context;
} mws_worker_command_t;

struct mws_transport_worker_s {
	uint32_t index;
	switch_mutex_t *mutex;
	switch_mutex_t *shutdown_mutex;
	switch_queue_t *commands;
	switch_thread_t *thread;
	CURLM *multi;
	mws_worker_command_t stop_command;
	switch_bool_t running;
	switch_bool_t stopping;
	switch_bool_t stop_queued;
	switch_bool_t cleanup_streams_on_stop;
	uint32_t regular_commands_queued;
	uint32_t assigned_streams;
};

typedef struct {
	char session_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char stream_id[SWITCH_UUID_FORMATTED_LENGTH + 1];
} mws_stream_identity_t;

/* API and cleanup queues identify streams by session and stream ID. */
typedef struct mws_api_command_task_s {
	mws_stream_identity_t identity;
	char command[MWS_MAXIMUM_TEXT_MESSAGE_BYTES + 1];
	struct mws_api_command_task_s *queued_next;
} mws_api_command_task_t;

typedef struct mws_cleanup_task_s {
	mws_stream_identity_t identity;
	struct mws_cleanup_task_s *queued_next;
} mws_cleanup_task_t;

struct mws_stream_s {
	switch_memory_pool_t *resource_pool;
	uint32_t resource_borrow_count;
	switch_bool_t resource_release_requested;
	switch_bool_t admission_reserved;
	switch_bool_t cleanup_holds_stream_slot;
	mws_channel_t *admission_channel_context;
	mws_channel_t *channel_context;
	CURL *easy;
	mws_transport_state_t transport_state;
	mws_transport_state_t transport_terminal_state;
	CURLcode curl_result;
	CURLMcode multi_result;
	char curl_error[CURL_ERROR_SIZE];
	switch_bool_t easy_added;
	switch_bool_t upload_paused;
	switch_bool_t receive_paused;
	uint32_t receive_pause_required_bytes;
	switch_bool_t normal_close_requested;
	switch_bool_t worker_removal_requested;
	switch_bool_t close_frame_started;
	switch_bool_t close_frame_sent;
	uint8_t close_frame_payload[MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES];
	switch_size_t close_frame_payload_length_bytes;
	switch_size_t close_frame_offset_bytes;
	switch_time_t normal_close_deadline;
	switch_bool_t terminal_result_reported;
	mws_captured_audio_buffer_t captured_audio_buffer;
	char *url;
	char *session_uuid;
	char stream_id[SWITCH_UUID_FORMATTED_LENGTH + 1];
	mws_stream_request_header_t *request_headers;
	struct curl_slist *curl_request_headers;
	char *start_message;
	switch_size_t start_message_length_bytes;
	switch_size_t start_message_offset_bytes;
	switch_bool_t start_message_frame_started;
	switch_bool_t start_message_sent;
	int target_sample_rate;
	mws_channel_to_websocket_t channel_to_websocket;
	mws_websocket_to_channel_t websocket_to_channel;
	int reconnect_attempt_limit;
	int reconnect_delay_ms;
	uint32_t consecutive_reconnect_attempts;
	uint64_t reconnect_attempt_count;
	uint64_t reconnect_success_count;
	switch_time_t next_reconnect_at;
	int statistics_interval_ms;
	uint32_t maximum_start_variables;
	switch_size_t maximum_start_variable_bytes;
	switch_size_t maximum_start_message_bytes;
	switch_size_t maximum_api_response_bytes;
	switch_bool_t api_commands_enabled;
	switch_bool_t peer_close_received;
	uint64_t captured_media_frame_count;
	uint64_t captured_native_byte_count;
	uint64_t media_callback_budget_hit_count;
	uint64_t libcurl_submitted_frame_count;
	uint64_t libcurl_submitted_byte_count;
	uint64_t text_frame_sent_count;
	uint64_t text_byte_sent_count;
	mws_captured_audio_resampler_t captured_audio_resampler;
	switch_bool_t captured_audio_resampler_initialized;
	uint64_t inbound_frame_count;
	uint64_t inbound_byte_count;
	uint64_t invalid_inbound_chunk_count;
	uint64_t inbound_binary_frame_count;
	uint64_t inbound_binary_byte_count;
	uint64_t inbound_text_frame_count;
	uint64_t inbound_text_byte_count;
	char *inbound_text_message;
	switch_size_t inbound_text_message_length_bytes;
	switch_bool_t inbound_text_message_active;
	switch_bool_t inbound_text_message_too_large;
	char inbound_text_message_first_byte;
	switch_mutex_t *outbound_message_mutex;
	char (*outbound_messages)[MWS_MAXIMUM_TEXT_MESSAGE_BYTES];
	switch_size_t *outbound_message_lengths;
	switch_size_t outbound_message_read_index;
	switch_size_t outbound_message_write_index;
	switch_size_t outbound_message_count;
	switch_size_t outbound_message_offset_bytes;
	switch_bool_t outbound_message_frame_started;
	_Atomic uint64_t outbound_message_sent_count;
	uint64_t dropped_outbound_message_count;
	char (*text_response_queue)[MWS_MAXIMUM_INTERNAL_RESPONSE_BYTES];
	switch_size_t *text_response_lengths;
	switch_size_t text_response_read_index;
	switch_size_t text_response_write_index;
	switch_size_t text_response_count;
	switch_size_t text_response_offset_bytes;
	switch_bool_t text_response_frame_started;
	switch_mutex_t *api_command_mutex;
	char *api_command_result;
	switch_size_t api_command_result_length_bytes;
	switch_size_t api_command_result_offset_bytes;
	switch_bool_t api_command_result_frame_started;
	switch_bool_t api_command_result_ready;
	switch_bool_t api_command_active;
	switch_bool_t clear_command_queued;
	switch_bool_t clear_command_complete;
	uint64_t clear_command_result_sample_count;
	uint64_t clear_command_count;
	uint64_t clear_discarded_sample_count;
	uint64_t api_command_received_count;
	uint64_t api_command_executed_count;
	uint64_t api_command_failed_count;
	uint64_t api_command_result_byte_count;
	_Atomic uint64_t rejected_api_command_count;
	_Atomic uint64_t message_event_count;
	_Atomic uint64_t rejected_message_count;
	uint64_t dropped_text_response_count;
	mws_playback_audio_buffer_t playback_audio_buffer;
	mws_source_audio_buffer_t source_audio_buffer;
	_Atomic uint32_t playback_sample_rate;
	_Atomic uint32_t playback_format_generation;
	_Atomic uint32_t playback_format_reset_requested;
	mws_playback_audio_resampler_t playback_audio_resampler;
	switch_bool_t playback_audio_resampler_initialized;
	mws_websocket_audio_input_t websocket_audio_input;
	_Atomic mws_playback_state_t playback_state;
	_Atomic uint64_t playback_received_sample_count;
	_Atomic uint64_t playback_source_dropped_sample_count;
	_Atomic uint64_t playback_buffered_sample_count;
	_Atomic uint64_t playback_mixed_sample_count;
	_Atomic uint64_t playback_dropped_sample_count;
	_Atomic uint64_t playback_invalid_frame_count;
	mws_playback_timing_t playback_timing;
	_Atomic uint64_t playback_deferred_callback_count;
	_Atomic uint32_t playback_peak_queued_bytes;
	_Atomic uint32_t playback_overflow_active;
	_Atomic uint64_t playback_format_change_count;
	switch_mutex_t *worker_state_mutex;
	switch_thread_cond_t *worker_state_condition;
	mws_transport_worker_t *transport_worker;
	switch_bool_t worker_assignment_counted;
	switch_bool_t worker_add_queued;
	switch_bool_t worker_remove_queued;
	switch_bool_t worker_attached;
	switch_bool_t worker_removed;
	_Atomic bool shutdown_requested;
	_Atomic bool startup_complete;
	_Atomic bool teardown_claimed;
	switch_atomic_t automatic_cleanup_requested;
	switch_time_t next_statistics_event_at;
	mws_worker_command_t worker_add_command;
	mws_worker_command_t worker_remove_command;
	mws_worker_command_t worker_clear_command;
	mws_stream_t *next;
	mws_stream_t *channel_next;
	mws_stream_t *deferred_cleanup_next;
	mws_stream_t *pending_cleanup_next;
	mws_stream_t *pending_callback_hold_next;
	switch_bool_t close_cleanup_owned;
	mws_stream_t *worker_next;
};

struct mws_channel_s {
	switch_media_bug_t *bug;
	switch_mutex_t *mutex;
	switch_thread_cond_t *condition;
	char *session_uuid;
	mws_stream_t *streams;
	uint32_t reserved_streams;
	mws_media_callback_clock_t read_playback_clock;
	mws_media_callback_clock_t write_playback_clock;
	switch_bool_t media_bug_add_in_progress;
	switch_bool_t closing;
};

typedef struct {
	mws_transport_state_t transport_state;
	mws_playback_state_t playback_state;
	switch_size_t captured_queued_bytes;
	switch_size_t captured_capacity_bytes;
	switch_size_t fallback_queued_bytes;
	switch_size_t playback_source_queued_bytes;
	switch_size_t playback_source_capacity_bytes;
	switch_size_t playback_queued_bytes;
	switch_size_t playback_capacity_bytes;
	switch_size_t playback_peak_queued_bytes;
	uint64_t captured_drop_events;
	uint64_t captured_dropped_bytes;
	uint32_t captured_sample_rate;
	uint32_t captured_channels;
	uint64_t captured_format_generation;
	uint64_t captured_invalid_frames;
	uint64_t captured_ring_lock_contentions;
	uint64_t captured_fallback_drops;
	uint64_t captured_fallback_stores;
	uint64_t captured_fallback_overwrites;
	uint64_t captured_fallback_overwritten_bytes;
	uint64_t captured_fallback_abandoned_bytes;
	uint64_t captured_stale_ring_discarded_bytes;
	uint64_t playback_received_samples;
	uint64_t playback_source_dropped_samples;
	uint64_t playback_buffered_samples;
	uint64_t playback_mixed_samples;
	uint64_t playback_dropped_samples;
	uint64_t playback_underflow_frames;
	uint64_t playback_invalid_frames;
	uint64_t playback_callbacks;
	uint64_t playback_requested_samples;
	uint64_t playback_callback_intervals;
	uint64_t playback_callback_interval_total_us;
	uint64_t playback_callback_max_interval_us;
	uint64_t playback_callback_max_lateness_us;
	uint64_t playback_callback_gap_intervals;
	uint64_t playback_module_dispatch_max_delay_us;
	uint64_t playback_underflow_with_source_audio_count;
	uint64_t playback_observation_snapshot_misses;
	bool playback_websocket_audio_seen;
	bool playback_buffer_write_seen;
	bool playback_worker_visit_seen;
	uint64_t playback_websocket_audio_age_us;
	uint64_t playback_buffer_write_age_us;
	uint64_t playback_worker_visit_age_us;
	mws_playback_timing_observation_t playback_maximum_gap;
	mws_playback_timing_observation_t playback_last_underflow;
	uint64_t playback_deferred_callbacks;
	uint64_t playback_queued_duration_us;
	uint64_t playback_rate_target_queue_us;
	uint64_t playback_rate_filtered_queue_us;
	int32_t playback_rate_adjustment_ppm;
	uint32_t playback_rate_maximum_adjustment_ppm;
	uint64_t playback_rate_updates;
	uint64_t playback_rate_limit_duration_us;
	uint32_t consecutive_reconnect_attempts;
	uint64_t reconnect_attempt_count;
	uint64_t reconnect_success_count;
	uint64_t reconnect_in_ms;
} mws_stream_statistics_t;

static void snapshot_stream_statistics(
		mws_stream_t *stream_context,
		mws_stream_statistics_t *statistics);

static struct {
	switch_mutex_t *config_mutex;
	switch_mutex_t *streams_mutex;
	switch_mutex_t *worker_mutex;
	switch_mutex_t *transport_assignment_mutex;
	mws_transport_worker_t *transport_workers;
	uint32_t transport_worker_count;
	uint32_t next_transport_worker;
	switch_bool_t transport_workers_stopping;
	switch_mutex_t *api_command_worker_mutex;
	switch_queue_t *api_command_tasks;
	switch_thread_t *api_command_worker_thread;
	mws_api_command_task_t api_command_stop_task;
	switch_bool_t api_command_worker_running;
	switch_bool_t api_command_worker_stopping;
	switch_bool_t api_command_stop_queued;
	uint32_t api_command_tasks_queued;
	mws_api_command_task_t *queued_api_command_tasks;
	switch_mutex_t *cleanup_worker_mutex;
	switch_thread_cond_t *cleanup_worker_condition;
	switch_queue_t *cleanup_tasks;
	switch_thread_t *cleanup_worker_thread;
	mws_cleanup_task_t cleanup_stop_task;
	mws_cleanup_task_t cleanup_wake_task;
	switch_bool_t cleanup_worker_running;
	switch_bool_t cleanup_worker_stopping;
	switch_bool_t cleanup_stop_queued;
	switch_bool_t cleanup_wake_queued;
	uint32_t cleanup_tasks_queued;
	mws_cleanup_task_t *queued_cleanup_tasks;
	mws_stream_t *pending_detached_cleanups;
	uint32_t pending_detached_cleanup_count;
	mws_stream_t *pending_callback_holds;
	uint32_t pending_callback_hold_count;
	uint32_t cleanup_callback_producers;
	uint32_t connecting_streams;
	uint32_t open_streams;
	uint32_t reconnecting_streams;
	uint32_t failed_streams;
	uint64_t reconnect_attempt_count;
	uint64_t reconnect_success_count;
	uint64_t reconnect_exhausted_count;
	uint64_t transport_event_count;
	uint64_t statistics_event_count;
	uint64_t error_event_count;
	uint64_t message_event_count;
	uint64_t automatic_cleanup_request_count;
	mws_stream_t *streams;
	uint32_t active_streams;
	uint32_t reserved_streams;
	int maximum_streams;
	int maximum_streams_per_channel;
	int maximum_http_headers;
	int maximum_http_header_bytes;
	int maximum_start_variables;
	int maximum_start_variable_bytes;
	int maximum_start_message_bytes;
	int maximum_api_response_bytes;
	switch_bool_t api_commands_enabled;
	int target_sample_rate;
	mws_channel_to_websocket_t channel_to_websocket;
	mws_websocket_to_channel_t websocket_to_channel;
	int reconnect_attempts;
	int reconnect_delay_ms;
	mws_http_header_t *configured_headers;
	uint32_t configured_header_count;
	int statistics_interval_ms;
	int configured_transport_workers;
} globals;

static switch_xml_config_enum_item_t channel_to_websocket_options[] = {
	{"read", MWS_CHANNEL_TO_WEBSOCKET_READ},
	{"write", MWS_CHANNEL_TO_WEBSOCKET_WRITE},
	{"mixed", MWS_CHANNEL_TO_WEBSOCKET_MIXED},
	{"stereo", MWS_CHANNEL_TO_WEBSOCKET_STEREO},
	{NULL, 0}
};

static switch_xml_config_enum_item_t websocket_to_channel_options[] = {
	{"write", MWS_WEBSOCKET_TO_CHANNEL_WRITE},
	{"read", MWS_WEBSOCKET_TO_CHANNEL_READ},
	{NULL, 0}
};

static switch_xml_config_int_options_t target_sample_rate_options = {
	SWITCH_TRUE,
	MWS_MINIMUM_SAMPLE_RATE,
	SWITCH_TRUE,
	MWS_MAXIMUM_SAMPLE_RATE
};

static switch_xml_config_int_options_t statistics_interval_options = {
	SWITCH_TRUE,
	0,
	SWITCH_TRUE,
	MWS_MAXIMUM_STATISTICS_INTERVAL_MS
};

static switch_xml_config_int_options_t reconnect_attempt_options = {
	SWITCH_TRUE,
	0,
	SWITCH_TRUE,
	MWS_MAXIMUM_RECONNECT_ATTEMPTS
};

static switch_xml_config_int_options_t reconnect_delay_options = {
	SWITCH_TRUE,
	MWS_MINIMUM_RECONNECT_DELAY_MS,
	SWITCH_TRUE,
	MWS_MAXIMUM_RECONNECT_DELAY_MS
};

static switch_xml_config_int_options_t transport_worker_options = {
	SWITCH_TRUE,
	0,
	SWITCH_TRUE,
	MWS_HARD_MAXIMUM_TRANSPORT_WORKERS
};

#define MWS_LIMIT_OPTIONS(name, minimum, maximum) \
	static switch_xml_config_int_options_t name = { \
		SWITCH_TRUE, minimum, SWITCH_TRUE, maximum \
	}

MWS_LIMIT_OPTIONS(maximum_streams_options, 1, MWS_HARD_MAXIMUM_STREAMS);
MWS_LIMIT_OPTIONS(
		maximum_streams_per_channel_options,
		1,
		MWS_HARD_MAXIMUM_STREAMS_PER_CHANNEL);
MWS_LIMIT_OPTIONS(
		maximum_http_headers_options,
		1,
		MWS_HARD_MAXIMUM_HTTP_HEADERS);
MWS_LIMIT_OPTIONS(
		maximum_http_header_bytes_options,
		1,
		MWS_HARD_MAXIMUM_HTTP_HEADER_BYTES);
MWS_LIMIT_OPTIONS(
		maximum_start_variables_options,
		1,
		MWS_HARD_MAXIMUM_START_VARIABLES);
MWS_LIMIT_OPTIONS(
		maximum_start_variable_bytes_options,
		1,
		MWS_HARD_MAXIMUM_START_VARIABLE_BYTES);
MWS_LIMIT_OPTIONS(
		maximum_start_message_bytes_options,
		1,
		MWS_HARD_MAXIMUM_START_MESSAGE_BYTES);
MWS_LIMIT_OPTIONS(
		maximum_api_response_bytes_options,
		MWS_MINIMUM_API_RESPONSE_BYTES,
		MWS_HARD_MAXIMUM_API_RESPONSE_BYTES);

#undef MWS_LIMIT_OPTIONS

static switch_xml_config_item_t config_instructions[] = {
	SWITCH_CONFIG_ITEM(
			"sample-rate",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.target_sample_rate,
			(void *) MWS_DEFAULT_SAMPLE_RATE,
			&target_sample_rate_options,
			"<integer-hz>",
			"WebSocket PCM sample rate in Hz; accepts any integer from "
			"8000 through 48000 (default: 16000)."),
	SWITCH_CONFIG_ITEM(
			"channel-to-websocket",
			SWITCH_CONFIG_ENUM,
			CONFIG_RELOADABLE,
			&globals.channel_to_websocket,
			(void *) MWS_CHANNEL_TO_WEBSOCKET_READ,
			&channel_to_websocket_options,
			"read|write|mixed|stereo",
			"Audio FreeSWITCH sends to the WebSocket: read, write, read/write "
			"mixed into mono, or two-channel PCM with read on the left and "
			"write on the right (default: read)."),
	SWITCH_CONFIG_ITEM(
			"websocket-to-channel",
			SWITCH_CONFIG_ENUM,
			CONFIG_RELOADABLE,
			&globals.websocket_to_channel,
			(void *) MWS_WEBSOCKET_TO_CHANNEL_WRITE,
			&websocket_to_channel_options,
			"write|read",
			"Where WebSocket audio enters the attached channel: write sends it "
			"toward the remote side; read treats it as audio arriving from the "
			"remote side (default: write)."),
	SWITCH_CONFIG_ITEM(
			"reconnect-attempts",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.reconnect_attempts,
			(void *) MWS_DEFAULT_RECONNECT_ATTEMPTS,
			&reconnect_attempt_options,
			"<integer-attempts>",
			"Maximum reconnection attempts after an unexpected transport "
			"failure; 0 disables reconnection (default: 5)."),
	SWITCH_CONFIG_ITEM(
			"reconnect-delay-ms",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.reconnect_delay_ms,
			(void *) MWS_DEFAULT_RECONNECT_DELAY_MS,
			&reconnect_delay_options,
			"<integer-milliseconds>",
			"Initial reconnection delay; each consecutive retry doubles the "
			"delay up to 30000 milliseconds (default: 1000)."),
	SWITCH_CONFIG_ITEM(
			"statistics-interval-ms",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.statistics_interval_ms,
			(void *) MWS_DEFAULT_STATISTICS_INTERVAL_MS,
			&statistics_interval_options,
			"<integer-milliseconds>",
			"Interval for per-stream statistics events; 0 disables them "
			"(default: 10000)."),
	SWITCH_CONFIG_ITEM(
			"transport-workers",
			SWITCH_CONFIG_INT,
			0,
			&globals.configured_transport_workers,
			(void *) 0,
			&transport_worker_options,
			"<integer-workers>",
			"Transport worker threads. 0 selects half the detected CPUs, "
			"capped at 8. This setting requires a module reload (default: 0)."),
	SWITCH_CONFIG_ITEM(
			"maximum-streams",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_streams,
			(void *) MWS_DEFAULT_MAXIMUM_STREAMS,
			&maximum_streams_options,
			"<integer-streams>",
			"Maximum streams across FreeSWITCH (default: 256)."),
	SWITCH_CONFIG_ITEM(
			"maximum-streams-per-channel",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_streams_per_channel,
			(void *) MWS_DEFAULT_MAXIMUM_STREAMS_PER_CHANNEL,
			&maximum_streams_per_channel_options,
			"<integer-streams>",
			"Maximum streams on one channel (default: 16)."),
	SWITCH_CONFIG_ITEM(
			"maximum-http-headers",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_http_headers,
			(void *) MWS_DEFAULT_MAXIMUM_HTTP_HEADERS,
			&maximum_http_headers_options,
			"<integer-headers>",
			"Maximum HTTP headers on a WebSocket request (default: 64)."),
	SWITCH_CONFIG_ITEM(
			"maximum-http-header-bytes",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_http_header_bytes,
			(void *) MWS_DEFAULT_MAXIMUM_HTTP_HEADER_BYTES,
			&maximum_http_header_bytes_options,
			"<integer-bytes>",
			"Maximum bytes across WebSocket request headers (default: 32768)."),
	SWITCH_CONFIG_ITEM(
			"maximum-start-variables",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_start_variables,
			(void *) MWS_DEFAULT_MAXIMUM_START_VARIABLES,
			&maximum_start_variables_options,
			"<integer-variables>",
			"Maximum channel variables in the start message (default: 256)."),
	SWITCH_CONFIG_ITEM(
			"maximum-start-variable-bytes",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_start_variable_bytes,
			(void *) MWS_DEFAULT_MAXIMUM_START_VARIABLE_BYTES,
			&maximum_start_variable_bytes_options,
			"<integer-bytes>",
			"Maximum bytes across start-message variables (default: 32768)."),
	SWITCH_CONFIG_ITEM(
			"maximum-start-message-bytes",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_start_message_bytes,
			(void *) MWS_DEFAULT_MAXIMUM_START_MESSAGE_BYTES,
			&maximum_start_message_bytes_options,
			"<integer-bytes>",
			"Maximum start-message size (default: 65536)."),
	SWITCH_CONFIG_ITEM(
			"maximum-api-response-bytes",
			SWITCH_CONFIG_INT,
			CONFIG_RELOADABLE,
			&globals.maximum_api_response_bytes,
			(void *) MWS_DEFAULT_MAXIMUM_API_RESPONSE_BYTES,
			&maximum_api_response_bytes_options,
			"<integer-bytes>",
			"Maximum API response returned over WebSocket (default: 65536)."),
	SWITCH_CONFIG_ITEM(
			"api-commands",
			SWITCH_CONFIG_BOOL,
			CONFIG_RELOADABLE,
			&globals.api_commands_enabled,
			(void *) SWITCH_TRUE,
			NULL,
			"true|false",
			"Execute plain-text WebSocket messages as FreeSWITCH API "
			"commands. JSON messages and clear are accepted either way "
			"(default: true)."),
	SWITCH_CONFIG_ITEM_END()
};

typedef struct {
	int target_sample_rate;
	mws_channel_to_websocket_t channel_to_websocket;
	mws_websocket_to_channel_t websocket_to_channel;
	int reconnect_attempts;
	int reconnect_delay_ms;
	int statistics_interval_ms;
	int configured_transport_workers;
	int maximum_streams;
	int maximum_streams_per_channel;
	int maximum_http_headers;
	int maximum_http_header_bytes;
	int maximum_start_variables;
	int maximum_start_variable_bytes;
	int maximum_start_message_bytes;
	int maximum_api_response_bytes;
	switch_bool_t api_commands_enabled;
} mws_config_snapshot_t;

static void snapshot_config_locked(mws_config_snapshot_t *snapshot)
{
	snapshot->target_sample_rate = globals.target_sample_rate;
	snapshot->channel_to_websocket = globals.channel_to_websocket;
	snapshot->websocket_to_channel = globals.websocket_to_channel;
	snapshot->reconnect_attempts = globals.reconnect_attempts;
	snapshot->reconnect_delay_ms = globals.reconnect_delay_ms;
	snapshot->statistics_interval_ms = globals.statistics_interval_ms;
	snapshot->configured_transport_workers =
			globals.configured_transport_workers;
	snapshot->maximum_streams = globals.maximum_streams;
	snapshot->maximum_streams_per_channel =
			globals.maximum_streams_per_channel;
	snapshot->maximum_http_headers = globals.maximum_http_headers;
	snapshot->maximum_http_header_bytes = globals.maximum_http_header_bytes;
	snapshot->maximum_start_variables = globals.maximum_start_variables;
	snapshot->maximum_start_variable_bytes =
			globals.maximum_start_variable_bytes;
	snapshot->maximum_start_message_bytes =
			globals.maximum_start_message_bytes;
	snapshot->maximum_api_response_bytes = globals.maximum_api_response_bytes;
	snapshot->api_commands_enabled = globals.api_commands_enabled;
}

static void restore_config_locked(const mws_config_snapshot_t *snapshot)
{
	globals.target_sample_rate = snapshot->target_sample_rate;
	globals.channel_to_websocket = snapshot->channel_to_websocket;
	globals.websocket_to_channel = snapshot->websocket_to_channel;
	globals.reconnect_attempts = snapshot->reconnect_attempts;
	globals.reconnect_delay_ms = snapshot->reconnect_delay_ms;
	globals.statistics_interval_ms = snapshot->statistics_interval_ms;
	globals.configured_transport_workers =
			snapshot->configured_transport_workers;
	globals.maximum_streams = snapshot->maximum_streams;
	globals.maximum_streams_per_channel =
			snapshot->maximum_streams_per_channel;
	globals.maximum_http_headers = snapshot->maximum_http_headers;
	globals.maximum_http_header_bytes = snapshot->maximum_http_header_bytes;
	globals.maximum_start_variables = snapshot->maximum_start_variables;
	globals.maximum_start_variable_bytes =
			snapshot->maximum_start_variable_bytes;
	globals.maximum_start_message_bytes =
			snapshot->maximum_start_message_bytes;
	globals.maximum_api_response_bytes = snapshot->maximum_api_response_bytes;
	globals.api_commands_enabled = snapshot->api_commands_enabled;
}

static switch_bool_t configured_limit_range(
		const char *name,
		long *minimum,
		long *maximum)
{
	*minimum = 1;
	if (!strcasecmp(name, "maximum-streams")) {
		*maximum = MWS_HARD_MAXIMUM_STREAMS;
	} else if (!strcasecmp(name, "maximum-streams-per-channel")) {
		*maximum = MWS_HARD_MAXIMUM_STREAMS_PER_CHANNEL;
	} else if (!strcasecmp(name, "maximum-http-headers")) {
		*maximum = MWS_HARD_MAXIMUM_HTTP_HEADERS;
	} else if (!strcasecmp(name, "maximum-http-header-bytes")) {
		*maximum = MWS_HARD_MAXIMUM_HTTP_HEADER_BYTES;
	} else if (!strcasecmp(name, "maximum-start-variables")) {
		*maximum = MWS_HARD_MAXIMUM_START_VARIABLES;
	} else if (!strcasecmp(name, "maximum-start-variable-bytes")) {
		*maximum = MWS_HARD_MAXIMUM_START_VARIABLE_BYTES;
	} else if (!strcasecmp(name, "maximum-start-message-bytes")) {
		*maximum = MWS_HARD_MAXIMUM_START_MESSAGE_BYTES;
	} else if (!strcasecmp(name, "maximum-api-response-bytes")) {
		*minimum = MWS_MINIMUM_API_RESPONSE_BYTES;
		*maximum = MWS_HARD_MAXIMUM_API_RESPONSE_BYTES;
	} else {
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

static switch_status_t validate_configured_limits(
		switch_xml_t configuration)
{
	switch_xml_t settings;
	switch_xml_t parameter;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	settings = switch_xml_child(configuration, "settings");
	if (settings) {
		for (parameter = switch_xml_child(settings, "param"); parameter;
				parameter = parameter->next) {
			const char *name = switch_xml_attr_soft(parameter, "name");
			const char *value = switch_xml_attr_soft(parameter, "value");
			char *end = NULL;
			long minimum;
			long maximum;
			long parsed;

			if (!configured_limit_range(name, &minimum, &maximum)) {
				continue;
			}
			errno = 0;
			parsed = strtol(value, &end, 10);
			if (errno || end == value || *end || parsed < minimum ||
					parsed > maximum) {
				switch_log_printf(
						SWITCH_CHANNEL_LOG,
						SWITCH_LOG_ERROR,
						"Invalid %s value '%s'; expected %ld through %ld\n",
						name,
						value,
						minimum,
						maximum);
				status = SWITCH_STATUS_FALSE;
				break;
			}
		}
	}

	return status;
}

static void copy_stream_identity(
		mws_stream_identity_t *identity,
		const mws_stream_t *stream_context)
{
	switch_copy_string(
			identity->session_uuid,
			stream_context->session_uuid,
			sizeof(identity->session_uuid));
	switch_copy_string(
			identity->stream_id,
			stream_context->stream_id,
			sizeof(identity->stream_id));
}

static switch_memory_pool_t *claim_stream_resource_pool_locked(
		mws_stream_t *stream_context)
{
	switch_memory_pool_t *pool;

	if (!stream_context->resource_release_requested ||
			stream_context->resource_borrow_count ||
			!stream_context->resource_pool) {
		return NULL;
	}

	pool = stream_context->resource_pool;
	stream_context->resource_pool = NULL;
	return pool;
}

static void destroy_stream_resource_pool(
		mws_stream_t *stream_context,
		switch_memory_pool_t *pool)
{
	if (!pool) {
		return;
	}

	if (stream_context->captured_audio_resampler_initialized) {
		mws_captured_audio_resampler_reset(
				&stream_context->captured_audio_resampler);
	}
	if (stream_context->playback_audio_resampler_initialized) {
		mws_playback_audio_resampler_reset(
				&stream_context->playback_audio_resampler);
	}
	switch_core_destroy_memory_pool(&pool);
}

static void request_stream_resource_release(mws_stream_t *stream_context)
{
	switch_memory_pool_t *pool;

	switch_mutex_lock(globals.streams_mutex);
	stream_context->resource_release_requested = SWITCH_TRUE;
	pool = claim_stream_resource_pool_locked(stream_context);
	switch_mutex_unlock(globals.streams_mutex);

	destroy_stream_resource_pool(stream_context, pool);
}

static void retain_stream_resource_borrow(mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.streams_mutex);
	switch_assert(stream_context->resource_pool);
	stream_context->resource_borrow_count++;
	switch_mutex_unlock(globals.streams_mutex);
}

static void release_stream_resource_borrow(mws_stream_t *stream_context)
{
	switch_memory_pool_t *pool;

	switch_mutex_lock(globals.streams_mutex);
	switch_assert(stream_context->resource_borrow_count > 0);
	stream_context->resource_borrow_count--;
	pool = claim_stream_resource_pool_locked(stream_context);
	switch_mutex_unlock(globals.streams_mutex);

	destroy_stream_resource_pool(stream_context, pool);
}

static switch_bool_t is_channel_header_variable(const char *name)
{
	return name && !strncasecmp(
			name,
			MWS_CHANNEL_HEADER_VARIABLE_PREFIX,
			strlen(MWS_CHANNEL_HEADER_VARIABLE_PREFIX));
}

static switch_status_t set_http_header(
		mws_http_header_t **headers,
		const char *name,
		const char *value,
		uint32_t maximum_headers,
		switch_size_t maximum_header_bytes)
{
	switch (mws_http_headers_set_limited(
			headers,
			name,
			value,
			maximum_headers,
			MWS_MAXIMUM_HTTP_HEADER_NAME_BYTES,
			MWS_MAXIMUM_HTTP_HEADER_VALUE_BYTES,
			maximum_header_bytes)) {
	case MWS_HTTP_HEADER_SET_OK:
		return SWITCH_STATUS_SUCCESS;
	case MWS_HTTP_HEADER_SET_NO_MEMORY:
		return SWITCH_STATUS_MEMERR;
	case MWS_HTTP_HEADER_SET_LIMIT_EXCEEDED:
	case MWS_HTTP_HEADER_SET_INVALID:
	default:
		return SWITCH_STATUS_FALSE;
	}
}

static switch_status_t parse_configured_headers(
		switch_xml_t configuration,
		mws_http_header_t **configured_headers,
		uint32_t maximum_headers,
		switch_size_t maximum_header_bytes)
{
	switch_xml_t headers;
	switch_xml_t header;
	mws_http_header_t *parsed_headers = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	for (headers = switch_xml_child(configuration, "headers"); headers;
			headers = headers->next) {
		for (header = switch_xml_child(headers, "header"); header;
				header = header->next) {
			const char *name = switch_xml_attr_soft(header, "name");
			const char *value = switch_xml_attr_soft(header, "value");

			status = set_http_header(
					&parsed_headers,
					name,
					value,
					maximum_headers,
					maximum_header_bytes);
			if (status != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(
						SWITCH_CHANNEL_LOG,
						SWITCH_LOG_ERROR,
						"Invalid or unallocatable WebSocket HTTP header "
						"in configuration: name=%s\n",
						zstr(name) ? "(empty)" : name);
				goto done;
			}
		}
	}

	*configured_headers = parsed_headers;
	parsed_headers = NULL;

done:
	mws_http_headers_free(parsed_headers);

	return status;
}

static switch_status_t build_stream_request_headers(
		switch_core_session_t *session,
		mws_stream_t *stream_context)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_event_header_t *variable;
	switch_bool_t channel_variables_locked = SWITCH_FALSE;
	mws_http_header_t *merged_headers = NULL;
	mws_http_header_t *header;
	mws_stream_request_header_t **stream_tail =
			&stream_context->request_headers;
	uint32_t maximum_headers;
	switch_size_t maximum_header_bytes;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_mutex_lock(globals.config_mutex);
	maximum_headers = (uint32_t) globals.maximum_http_headers;
	maximum_header_bytes =
			(switch_size_t) globals.maximum_http_header_bytes;
	for (header = globals.configured_headers; header; header = header->next) {
		status = set_http_header(
				&merged_headers,
				header->name,
				header->value,
				maximum_headers,
				maximum_header_bytes);
		if (status != SWITCH_STATUS_SUCCESS) {
			break;
		}
	}
	switch_mutex_unlock(globals.config_mutex);
	if (status != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	variable = switch_channel_variable_first(channel);
	channel_variables_locked = SWITCH_TRUE;
	for (; variable; variable = variable->next) {
		const char *header_name;

		if (!is_channel_header_variable(variable->name)) {
			continue;
		}

		header_name = variable->name +
				strlen(MWS_CHANNEL_HEADER_VARIABLE_PREFIX);
		status = set_http_header(
				&merged_headers,
				header_name,
				variable->value ? variable->value : "",
				maximum_headers,
				maximum_header_bytes);
		if (status != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(session),
					SWITCH_LOG_ERROR,
					"Invalid or unallocatable WebSocket HTTP header "
					"channel variable: %s\n",
					variable->name);
			goto done;
		}
	}
	switch_channel_variable_last(channel);
	channel_variables_locked = SWITCH_FALSE;

	for (header = merged_headers; header; header = header->next) {
		mws_stream_request_header_t *stream_header =
				switch_core_alloc(
				stream_context->resource_pool,
				sizeof(*stream_header));

		if (!stream_header ||
				!(stream_header->name = switch_core_strdup(
					stream_context->resource_pool,
					header->name)) ||
				!(stream_header->value = switch_core_strdup(
					stream_context->resource_pool,
					header->value))) {
			status = SWITCH_STATUS_MEMERR;
			goto done;
		}
		*stream_tail = stream_header;
		stream_tail = &stream_header->next;
	}

done:
	if (channel_variables_locked) {
		switch_channel_variable_last(channel);
	}
	mws_http_headers_free(merged_headers);

	return status;
}

static switch_media_bug_flag_t media_bug_flags(void)
{
	return SMBF_FIRST | SMBF_READ_PING | SMBF_READ_STREAM | SMBF_WRITE_STREAM |
			SMBF_STEREO | SMBF_READ_REPLACE | SMBF_WRITE_REPLACE;
}

static switch_size_t calculate_pcm_capacity_bytes(
		uint32_t sample_rate_hz,
		uint32_t channels,
		uint32_t duration_ms)
{
	uint64_t sample_count;

	if (sample_rate_hz < MWS_MINIMUM_SAMPLE_RATE ||
			sample_rate_hz > MWS_MAXIMUM_SAMPLE_RATE ||
			!channels || channels > MWS_MAXIMUM_OUTBOUND_CHANNELS ||
			!duration_ms) {
		return 0;
	}

	sample_count = (uint64_t) sample_rate_hz * duration_ms / 1000;

	return (switch_size_t) (
			sample_count * channels * sizeof(int16_t));
}

static switch_status_t build_start_message(
		switch_core_session_t *session,
		mws_stream_t *stream_context)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_event_header_t *header;
	switch_bool_t channel_variables_locked = SWITCH_FALSE;
	cJSON *root = NULL;
	cJSON *audio = NULL;
	cJSON *variables = NULL;
	char *serialized = NULL;
	uint32_t variable_count = 0;
	switch_size_t variable_bytes = 0;
	const char *channel_to_websocket;
	const char *websocket_to_channel;
	mws_captured_audio_buffer_statistics_t captured_audio_statistics;
	switch_status_t status = SWITCH_STATUS_FALSE;

	root = cJSON_CreateObject();
	audio = cJSON_CreateObject();
	variables = cJSON_CreateObject();
	if (!root || !audio || !variables) {
		goto done;
	}

	channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);
	mws_captured_audio_buffer_snapshot(
			&stream_context->captured_audio_buffer,
			&captured_audio_statistics);

	if (!cJSON_AddStringToObject(root, "event", "start") ||
			!cJSON_AddNumberToObject(root, "version", 1) ||
			!cJSON_AddStringToObject(
				root,
				"stream_id",
				stream_context->stream_id) ||
			!cJSON_AddStringToObject(
				root,
				"channel_uuid",
				stream_context->session_uuid) ||
			!cJSON_AddBoolToObject(
				root,
				"api_commands",
				stream_context->api_commands_enabled ? 1 : 0) ||
			!cJSON_AddStringToObject(audio, "format", "s16le") ||
			!cJSON_AddNumberToObject(
				audio,
				"sample_rate",
				stream_context->target_sample_rate) ||
			!cJSON_AddNumberToObject(
				audio,
				"channels",
				captured_audio_statistics.channels) ||
			!cJSON_AddStringToObject(
				audio,
				"channel_to_websocket",
				channel_to_websocket ? channel_to_websocket : "unknown") ||
			!cJSON_AddStringToObject(
				audio,
				"websocket_to_channel",
				websocket_to_channel ? websocket_to_channel : "unknown")) {
		goto done;
	}

	header = switch_channel_variable_first(channel);
	channel_variables_locked = SWITCH_TRUE;
	for (; header; header = header->next) {
		const char *value;
		switch_size_t name_bytes;
		switch_size_t value_bytes;

		if (zstr(header->name) ||
				is_channel_header_variable(header->name)) {
			continue;
		}
		value = header->value ? header->value : "";
		name_bytes = mws_bounded_string_length(
				header->name,
				MWS_MAXIMUM_START_VARIABLE_NAME_BYTES);
		value_bytes = mws_bounded_string_length(
				value,
				MWS_MAXIMUM_START_VARIABLE_VALUE_BYTES);
		if (name_bytes > MWS_MAXIMUM_START_VARIABLE_NAME_BYTES ||
				value_bytes > MWS_MAXIMUM_START_VARIABLE_VALUE_BYTES ||
				name_bytes > SIZE_MAX - value_bytes ||
				!mws_resource_budget_add(
						&variable_count,
						&variable_bytes,
						name_bytes + value_bytes,
						stream_context->maximum_start_variables,
						stream_context->maximum_start_variable_bytes)) {
			switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(session),
					SWITCH_LOG_ERROR,
					"Channel variables exceed the WebSocket start "
					"message limit\n");
			goto done;
		}
		if (!cJSON_AddStringToObject(
				variables,
				header->name,
				value)) {
			goto done;
		}
	}
	switch_channel_variable_last(channel);
	channel_variables_locked = SWITCH_FALSE;

	cJSON_AddItemToObject(root, "audio", audio);
	audio = NULL;
	cJSON_AddItemToObject(root, "channel_variables", variables);
	variables = NULL;

	serialized = malloc(stream_context->maximum_start_message_bytes + 5);
	if (!serialized || !cJSON_PrintPreallocated(
			root,
			serialized,
			(int) stream_context->maximum_start_message_bytes + 5,
			0) || strlen(serialized) >
			stream_context->maximum_start_message_bytes) {
		goto done;
	}

	stream_context->start_message = switch_core_strdup(
			stream_context->resource_pool,
			serialized);
	if (!stream_context->start_message) {
		goto done;
	}
	stream_context->start_message_length_bytes = strlen(serialized);
	status = SWITCH_STATUS_SUCCESS;

done:
	if (channel_variables_locked) {
		switch_channel_variable_last(channel);
	}
	if (serialized) {
		free(serialized);
	}
	if (audio) {
		cJSON_Delete(audio);
	}
	if (variables) {
		cJSON_Delete(variables);
	}
	if (root) {
		cJSON_Delete(root);
	}

	return status;
}

static switch_status_t initialize_stream_context(
		switch_core_session_t *session,
		mws_stream_t *stream_context)
{
	switch_codec_implementation_t read_implementation = { 0 };
	switch_codec_implementation_t write_implementation = { 0 };
	switch_codec_implementation_t *playback_implementation;
	switch_memory_pool_t *resource_pool = stream_context->resource_pool;
	uint32_t native_channels;
	switch_size_t native_ring_allocation_bytes;
	switch_size_t native_ring_capacity_bytes;
	switch_size_t native_chunk_capacity_bytes;
	switch_size_t pending_target_pcm_capacity_bytes;
	switch_size_t playback_audio_buffer_allocation_bytes;
	switch_size_t playback_audio_buffer_capacity_bytes;
	switch_size_t source_audio_buffer_capacity_bytes;
	switch_size_t playback_resampler_source_capacity_bytes;
	switch_size_t playback_resampler_output_capacity_bytes;
	uint8_t *captured_ring_storage;
	uint8_t *captured_fallback_storage;
	uint8_t *captured_source_storage;
	uint8_t *captured_target_storage;
	uint8_t *playback_audio_storage;
	uint8_t *source_audio_storage;
	uint8_t *playback_resampler_source_storage;
	uint8_t *playback_resampler_output_storage;
	uint8_t *websocket_audio_conversion_storage;

	if (!resource_pool) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_session_get_read_impl(session, &read_implementation);
	switch_core_session_get_write_impl(session, &write_implementation);
	playback_implementation =
			stream_context->websocket_to_channel ==
			MWS_WEBSOCKET_TO_CHANNEL_READ ?
			&read_implementation : &write_implementation;

	native_channels = read_implementation.number_of_channels;
	if (stream_context->channel_to_websocket ==
			MWS_CHANNEL_TO_WEBSOCKET_STEREO) {
		native_channels = 2;
	}

	native_ring_capacity_bytes = calculate_pcm_capacity_bytes(
			read_implementation.actual_samples_per_second,
			native_channels,
			MWS_NATIVE_RING_DURATION_MS);
	native_ring_allocation_bytes = calculate_pcm_capacity_bytes(
			MWS_MAXIMUM_SAMPLE_RATE,
			MWS_MAXIMUM_OUTBOUND_CHANNELS,
			MWS_NATIVE_RING_DURATION_MS);
	native_chunk_capacity_bytes = calculate_pcm_capacity_bytes(
			MWS_MAXIMUM_SAMPLE_RATE,
			MWS_MAXIMUM_OUTBOUND_CHANNELS,
			MWS_NATIVE_CHUNK_DURATION_MS);
	pending_target_pcm_capacity_bytes = native_chunk_capacity_bytes *
			MWS_MAXIMUM_SAMPLE_RATE / MWS_MINIMUM_SAMPLE_RATE;
	playback_audio_buffer_allocation_bytes = calculate_pcm_capacity_bytes(
			MWS_MAXIMUM_SAMPLE_RATE,
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	if (playback_audio_buffer_allocation_bytes) {
		playback_audio_buffer_allocation_bytes += sizeof(int16_t);
	}
	playback_audio_buffer_capacity_bytes = calculate_pcm_capacity_bytes(
			playback_implementation->actual_samples_per_second,
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	source_audio_buffer_capacity_bytes = calculate_pcm_capacity_bytes(
			(uint32_t) stream_context->target_sample_rate,
			1,
			MWS_PLAYBACK_SOURCE_RING_DURATION_MS);
	playback_resampler_source_capacity_bytes = calculate_pcm_capacity_bytes(
			MWS_MAXIMUM_SAMPLE_RATE,
			1,
			MWS_PLAYBACK_CHUNK_DURATION_MS);
	playback_resampler_output_capacity_bytes = calculate_pcm_capacity_bytes(
			MWS_MAXIMUM_SAMPLE_RATE,
			1,
			MWS_PLAYBACK_CHUNK_DURATION_MS);

	if (!native_ring_capacity_bytes || !native_ring_allocation_bytes ||
			!native_chunk_capacity_bytes ||
			!pending_target_pcm_capacity_bytes ||
			!playback_audio_buffer_allocation_bytes ||
			!playback_audio_buffer_capacity_bytes ||
			!source_audio_buffer_capacity_bytes ||
			!playback_resampler_source_capacity_bytes ||
			!playback_resampler_output_capacity_bytes) {
		return SWITCH_STATUS_FALSE;
	}

	atomic_init(
			&stream_context->playback_sample_rate,
			playback_implementation->actual_samples_per_second);
	atomic_init(&stream_context->playback_format_generation, 1);
	atomic_init(&stream_context->playback_format_reset_requested, 0);
	atomic_init(
			&stream_context->playback_state,
			MWS_PLAYBACK_BUFFERING);
	atomic_init(&stream_context->playback_received_sample_count, 0);
	atomic_init(&stream_context->playback_source_dropped_sample_count, 0);
	atomic_init(&stream_context->playback_buffered_sample_count, 0);
	atomic_init(&stream_context->playback_mixed_sample_count, 0);
	atomic_init(&stream_context->playback_dropped_sample_count, 0);
	atomic_init(&stream_context->playback_invalid_frame_count, 0);
	mws_playback_timing_init(&stream_context->playback_timing);
	atomic_init(&stream_context->playback_deferred_callback_count, 0);
	atomic_init(&stream_context->playback_peak_queued_bytes, 0);
	atomic_init(&stream_context->playback_overflow_active, 0);
	atomic_init(&stream_context->playback_format_change_count, 0);
	atomic_init(&stream_context->rejected_api_command_count, 0);
	atomic_init(&stream_context->message_event_count, 0);
	atomic_init(&stream_context->rejected_message_count, 0);
	atomic_init(&stream_context->shutdown_requested, false);
	atomic_init(&stream_context->startup_complete, false);
	atomic_init(&stream_context->teardown_claimed, false);
	if (switch_mutex_init(
				&stream_context->worker_state_mutex,
				SWITCH_MUTEX_UNNESTED,
				resource_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(
				&stream_context->api_command_mutex,
				SWITCH_MUTEX_UNNESTED,
				resource_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(
				&stream_context->outbound_message_mutex,
				SWITCH_MUTEX_UNNESTED,
				resource_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	atomic_init(&stream_context->outbound_message_sent_count, 0);

	if (switch_thread_cond_create(
				&stream_context->worker_state_condition,
				resource_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	stream_context->worker_add_command.type = MWS_WORKER_COMMAND_ADD;
	stream_context->worker_add_command.stream_context = stream_context;
	stream_context->worker_remove_command.type = MWS_WORKER_COMMAND_REMOVE;
	stream_context->worker_remove_command.stream_context = stream_context;
	stream_context->worker_clear_command.type = MWS_WORKER_COMMAND_CLEAR;
	stream_context->worker_clear_command.stream_context = stream_context;

	stream_context->inbound_text_message = switch_core_alloc(
			resource_pool,
			MWS_MAXIMUM_TEXT_MESSAGE_BYTES + 1);
	stream_context->text_response_queue = switch_core_alloc(
			resource_pool,
			MWS_TEXT_RESPONSE_QUEUE_CAPACITY *
					MWS_MAXIMUM_INTERNAL_RESPONSE_BYTES);
	stream_context->text_response_lengths = switch_core_alloc(
			resource_pool,
			MWS_TEXT_RESPONSE_QUEUE_CAPACITY *
					sizeof(*stream_context->text_response_lengths));
	stream_context->outbound_messages = switch_core_alloc(
			resource_pool,
			MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY *
					MWS_MAXIMUM_TEXT_MESSAGE_BYTES);
	stream_context->outbound_message_lengths = switch_core_alloc(
			resource_pool,
			MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY *
					sizeof(*stream_context->outbound_message_lengths));

	captured_ring_storage = switch_core_alloc(
			resource_pool,
			native_ring_allocation_bytes);
	captured_source_storage = switch_core_alloc(
			resource_pool,
			native_chunk_capacity_bytes);
	captured_target_storage = switch_core_alloc(
			resource_pool,
			pending_target_pcm_capacity_bytes);
	captured_fallback_storage = switch_core_alloc(
			resource_pool,
			native_chunk_capacity_bytes);
	if (mws_captured_audio_buffer_initialize(
			&stream_context->captured_audio_buffer,
			captured_ring_storage,
			native_ring_allocation_bytes,
			native_ring_capacity_bytes,
			captured_fallback_storage,
			native_chunk_capacity_bytes,
			read_implementation.actual_samples_per_second,
			native_channels,
			MWS_NATIVE_RING_DURATION_MS,
			MWS_NATIVE_CHUNK_DURATION_MS,
			resource_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	if (mws_captured_audio_resampler_initialize(
			&stream_context->captured_audio_resampler,
			captured_source_storage,
			native_chunk_capacity_bytes,
			captured_target_storage,
			pending_target_pcm_capacity_bytes,
			(uint32_t) stream_context->target_sample_rate,
			MWS_MINIMUM_SAMPLE_RATE,
			MWS_MAXIMUM_SAMPLE_RATE,
			MWS_MAXIMUM_OUTBOUND_CHANNELS,
			stream_context->session_uuid) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	stream_context->captured_audio_resampler_initialized = SWITCH_TRUE;
	playback_audio_storage = switch_core_alloc(
			resource_pool,
			playback_audio_buffer_allocation_bytes);
	mws_playback_audio_buffer_initialize(
			&stream_context->playback_audio_buffer,
			playback_audio_storage,
			(uint32_t) playback_audio_buffer_allocation_bytes,
			(uint32_t) playback_audio_buffer_capacity_bytes);
	source_audio_storage = switch_core_alloc(
			resource_pool,
			source_audio_buffer_capacity_bytes);
	mws_source_audio_buffer_initialize(
			&stream_context->source_audio_buffer,
			source_audio_storage,
			(uint32_t) source_audio_buffer_capacity_bytes);
	playback_resampler_source_storage = switch_core_alloc(
			resource_pool,
			playback_resampler_source_capacity_bytes);
	playback_resampler_output_storage = switch_core_alloc(
			resource_pool,
			playback_resampler_output_capacity_bytes);
	websocket_audio_conversion_storage = switch_core_alloc(
			resource_pool,
			playback_resampler_source_capacity_bytes);
	if (mws_playback_audio_resampler_initialize(
			&stream_context->playback_audio_resampler,
			playback_resampler_source_storage,
			(uint32_t) playback_resampler_source_capacity_bytes,
			playback_resampler_output_storage,
			(uint32_t) playback_resampler_output_capacity_bytes,
			(uint32_t) stream_context->target_sample_rate,
			MWS_MINIMUM_SAMPLE_RATE,
			MWS_MAXIMUM_SAMPLE_RATE,
			MWS_PLAYBACK_START_BUFFER_MS * 1000,
			MWS_PLAYBACK_RATE_UPDATE_INTERVAL_US,
			stream_context->session_uuid) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	stream_context->playback_audio_resampler_initialized = SWITCH_TRUE;
	if (mws_websocket_audio_input_initialize(
			&stream_context->websocket_audio_input,
			websocket_audio_conversion_storage,
			(uint32_t) playback_resampler_source_capacity_bytes) !=
			SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return stream_context->inbound_text_message &&
			stream_context->text_response_queue &&
			stream_context->text_response_lengths &&
			stream_context->playback_audio_buffer.data &&
			stream_context->source_audio_buffer.data ?
			SWITCH_STATUS_SUCCESS : SWITCH_STATUS_MEMERR;
}

static void worker_acknowledge_add(mws_stream_t *stream_context)
{
	switch_mutex_lock(stream_context->worker_state_mutex);
	stream_context->worker_attached = SWITCH_TRUE;
	stream_context->worker_removed = SWITCH_FALSE;
	switch_thread_cond_broadcast(stream_context->worker_state_condition);
	switch_mutex_unlock(stream_context->worker_state_mutex);
}

static switch_bool_t worker_claim_assignment_locked(
		mws_stream_t *stream_context)
{
	switch_bool_t claimed = stream_context->worker_assignment_counted;

	stream_context->worker_assignment_counted = SWITCH_FALSE;

	return claimed;
}

static void worker_acknowledge_remove(mws_stream_t *stream_context)
{
	mws_transport_worker_t *worker;
	switch_bool_t release_assignment;

	switch_mutex_lock(stream_context->worker_state_mutex);
	stream_context->worker_attached = SWITCH_FALSE;
	stream_context->worker_remove_queued = SWITCH_FALSE;
	stream_context->worker_removed = SWITCH_TRUE;
	worker = stream_context->transport_worker;
	release_assignment = worker_claim_assignment_locked(stream_context);
	switch_thread_cond_broadcast(stream_context->worker_state_condition);
	switch_mutex_unlock(stream_context->worker_state_mutex);

	if (worker && release_assignment) {
		switch_mutex_lock(worker->mutex);
		switch_assert(worker->assigned_streams > 0);
		worker->assigned_streams--;
		switch_mutex_unlock(worker->mutex);
	}
}

static switch_bool_t transport_worker_is_running(
		mws_transport_worker_t *worker)
{
	switch_bool_t running;

	switch_mutex_lock(worker->mutex);
	running = worker->running;
	switch_mutex_unlock(worker->mutex);

	return running;
}

static mws_transport_worker_t *reserve_transport_worker(void)
{
	mws_transport_worker_t *selected_worker = NULL;
	uint32_t selected_load = UINT32_MAX;
	uint32_t start;
	uint32_t offset;

	switch_mutex_lock(globals.transport_assignment_mutex);
	if (globals.transport_workers_stopping ||
			!globals.transport_workers || !globals.transport_worker_count) {
		switch_mutex_unlock(globals.transport_assignment_mutex);
		return NULL;
	}

	start = globals.next_transport_worker++ % globals.transport_worker_count;
	for (offset = 0; offset < globals.transport_worker_count; offset++) {
		mws_transport_worker_t *worker = &globals.transport_workers[
				(start + offset) % globals.transport_worker_count];

		switch_mutex_lock(worker->mutex);
		if (worker->running && !worker->stopping &&
				worker->assigned_streams < selected_load) {
			selected_worker = worker;
			selected_load = worker->assigned_streams;
		}
		switch_mutex_unlock(worker->mutex);
	}

	if (selected_worker) {
		switch_mutex_lock(selected_worker->mutex);
		if (selected_worker->running && !selected_worker->stopping) {
			selected_worker->assigned_streams++;
		} else {
			switch_mutex_unlock(selected_worker->mutex);
			selected_worker = NULL;
			goto selection_complete;
		}
		switch_mutex_unlock(selected_worker->mutex);
	}

selection_complete:
	switch_mutex_unlock(globals.transport_assignment_mutex);

	return selected_worker;
}

static void release_transport_worker_reservation(
		mws_transport_worker_t *worker)
{
	if (!worker) {
		return;
	}

	switch_mutex_lock(worker->mutex);
	switch_assert(worker->assigned_streams > 0);
	worker->assigned_streams--;
	switch_mutex_unlock(worker->mutex);
}

static switch_bool_t worker_command_uses_regular_slot(
		const mws_worker_command_t *command)
{
	return command && (command->type == MWS_WORKER_COMMAND_ADD ||
			command->type == MWS_WORKER_COMMAND_CLEAR);
}

static switch_bool_t worker_command_is_known(
		const mws_worker_command_t *command)
{
	if (!command) {
		return SWITCH_FALSE;
	}

	switch (command->type) {
	case MWS_WORKER_COMMAND_ADD:
	case MWS_WORKER_COMMAND_REMOVE:
	case MWS_WORKER_COMMAND_CLEAR:
	case MWS_WORKER_COMMAND_STOP:
		return SWITCH_TRUE;
	default:
		return SWITCH_FALSE;
	}
}

static void release_regular_worker_command_slot(
		mws_transport_worker_t *worker,
		const mws_worker_command_t *command)
{
	if (!worker_command_uses_regular_slot(command)) {
		return;
	}

	switch_mutex_lock(worker->mutex);
	switch_assert(worker->regular_commands_queued > 0);
	worker->regular_commands_queued--;
	switch_mutex_unlock(worker->mutex);
}

static switch_status_t push_worker_command(
		mws_transport_worker_t *worker,
		mws_worker_command_t *command)
{
	switch_bool_t regular = worker_command_uses_regular_slot(command);
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!worker_command_is_known(command)) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(worker->mutex);
	if (!worker->running) {
		status = SWITCH_STATUS_FALSE;
	} else if (worker->stopping &&
			command->type != MWS_WORKER_COMMAND_REMOVE &&
			command->type != MWS_WORKER_COMMAND_STOP) {
		status = SWITCH_STATUS_FALSE;
	} else if (command->type == MWS_WORKER_COMMAND_STOP &&
			worker->stop_queued) {
		status = SWITCH_STATUS_SUCCESS;
	} else if (!regular || worker->regular_commands_queued <
			MWS_MAXIMUM_QUEUED_REGULAR_WORKER_COMMANDS) {
		if (regular) {
			worker->regular_commands_queued++;
		}
		status = switch_queue_trypush(worker->commands, command);
		if (status != SWITCH_STATUS_SUCCESS && regular) {
			worker->regular_commands_queued--;
		} else if (status == SWITCH_STATUS_SUCCESS &&
				command->type == MWS_WORKER_COMMAND_STOP) {
			worker->stop_queued = SWITCH_TRUE;
		}
	}
	switch_mutex_unlock(worker->mutex);

	return status;
}

static const char *transport_state_name(mws_transport_state_t state)
{
	switch (state) {
	case MWS_TRANSPORT_NEW:
		return "new";
	case MWS_TRANSPORT_CONNECTING:
		return "connecting";
	case MWS_TRANSPORT_OPEN:
		return "open";
	case MWS_TRANSPORT_RECONNECTING:
		return "reconnecting";
	case MWS_TRANSPORT_CLOSED:
		return "closed";
	case MWS_TRANSPORT_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}

static const char *playback_state_name(mws_playback_state_t state)
{
	switch (state) {
	case MWS_PLAYBACK_BUFFERING:
		return "buffering";
	case MWS_PLAYBACK_PLAYING:
		return "playing";
	default:
		return "unknown";
	}
}

static void add_playback_rate_event_headers(
		switch_event_t *event,
		const mws_stream_statistics_t *statistics)
{
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Queued-Duration-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_queued_duration_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Target-Queue-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_rate_target_queue_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Filtered-Queue-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_rate_filtered_queue_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Adjustment-Ppm",
			"%d",
			statistics->playback_rate_adjustment_ppm);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Maximum-Adjustment-Ppm",
			"%u",
			statistics->playback_rate_maximum_adjustment_ppm);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Updates",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_rate_updates);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Rate-Limit-Duration-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_rate_limit_duration_us);
}

static void add_playback_timing_event_headers(
		switch_event_t *event,
		const mws_stream_statistics_t *statistics)
{
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Max-Lateness-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_callback_max_lateness_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Gap-Intervals",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_callback_gap_intervals);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Module-Dispatch-Max-Delay-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_module_dispatch_max_delay_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-With-Source-Audio-Count",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_underflow_with_source_audio_count);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Observation-Snapshot-Misses",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_observation_snapshot_misses);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-WebSocket-Audio-Seen",
			statistics->playback_websocket_audio_seen ? "true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Buffer-Write-Seen",
			statistics->playback_buffer_write_seen ? "true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Worker-Visit-Seen",
			statistics->playback_worker_visit_seen ? "true" : "false");
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-WebSocket-Audio-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_websocket_audio_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Buffer-Write-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_buffer_write_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Worker-Visit-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_worker_visit_age_us);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Observation-Valid",
			statistics->playback_maximum_gap.valid ? "true" : "false");
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Observed-At-Monotonic-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.observed_at_monotonic_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.observation_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Callback-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.callback_entry_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Expected-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.callback_expected_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Lateness-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.callback_lateness_us);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-State",
			playback_state_name((mws_playback_state_t)
				statistics->playback_maximum_gap.playback_state));
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Dispatch-Delay-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.module_dispatch_delay_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Source-Queued-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.source_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Queued-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.playback_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-WebSocket-Audio-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.websocket_audio_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Buffer-Write-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.playback_buffer_write_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Worker-Visit-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_maximum_gap.worker_visit_age_us);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-WebSocket-Audio-Seen",
			statistics->playback_maximum_gap.websocket_audio_seen ?
				"true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Buffer-Write-Seen",
			statistics->playback_maximum_gap.playback_buffer_write_seen ?
				"true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Max-Gap-Worker-Visit-Seen",
			statistics->playback_maximum_gap.worker_visit_seen ?
				"true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Observation-Valid",
			statistics->playback_last_underflow.valid ? "true" : "false");
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Observed-At-Monotonic-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.observed_at_monotonic_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.observation_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Callback-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.callback_entry_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Expected-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.callback_expected_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Callback-Lateness-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.callback_lateness_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Dispatch-Delay-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.module_dispatch_delay_us);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-State",
			playback_state_name((mws_playback_state_t)
				statistics->playback_last_underflow.playback_state));
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Source-Queued-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.source_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Queued-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.playback_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-WebSocket-Audio-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.websocket_audio_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Buffer-Write-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.playback_buffer_write_age_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Worker-Visit-Age-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics->playback_last_underflow.worker_visit_age_us);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-WebSocket-Audio-Seen",
			statistics->playback_last_underflow.websocket_audio_seen ?
				"true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Buffer-Write-Seen",
			statistics->playback_last_underflow.playback_buffer_write_seen ?
				"true" : "false");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Worker-Visit-Seen",
			statistics->playback_last_underflow.worker_visit_seen ?
				"true" : "false");
}

static void worker_set_transport_state(
		mws_stream_t *stream_context,
		mws_transport_state_t state)
{
	if (stream_context->transport_state == state) {
		return;
	}

	switch_mutex_lock(globals.worker_mutex);

	switch (stream_context->transport_state) {
	case MWS_TRANSPORT_CONNECTING:
		if (globals.connecting_streams > 0) {
			globals.connecting_streams--;
		}
		break;
	case MWS_TRANSPORT_OPEN:
		if (globals.open_streams > 0) {
			globals.open_streams--;
		}
		break;
	case MWS_TRANSPORT_RECONNECTING:
		if (globals.reconnecting_streams > 0) {
			globals.reconnecting_streams--;
		}
		break;
	case MWS_TRANSPORT_FAILED:
		if (globals.failed_streams > 0) {
			globals.failed_streams--;
		}
		break;
	case MWS_TRANSPORT_NEW:
	case MWS_TRANSPORT_CLOSED:
	default:
		break;
	}

	switch (state) {
	case MWS_TRANSPORT_CONNECTING:
		globals.connecting_streams++;
		break;
	case MWS_TRANSPORT_OPEN:
		globals.open_streams++;
		break;
	case MWS_TRANSPORT_RECONNECTING:
		globals.reconnecting_streams++;
		break;
	case MWS_TRANSPORT_FAILED:
		globals.failed_streams++;
		break;
	case MWS_TRANSPORT_NEW:
	case MWS_TRANSPORT_CLOSED:
	default:
		break;
	}

	stream_context->transport_state = state;
	switch_mutex_unlock(globals.worker_mutex);
}

/* Adds the channel's event data, or just Unique-ID once the session is gone. */
static void add_stream_channel_event_data(
		switch_event_t *event,
		mws_stream_t *stream_context)
{
	switch_core_session_t *session =
			switch_core_session_locate(stream_context->session_uuid);

	if (session) {
		switch_channel_event_set_data(
				switch_core_session_get_channel(session),
				event);
		switch_core_session_rwunlock(session);
		return;
	}
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"Unique-ID",
			stream_context->session_uuid);
}

/* Reports who closed the connection and, for the peer, its close frame. */
static void add_close_event_headers(
		switch_event_t *event,
		mws_stream_t *stream_context)
{
	const uint8_t *payload = stream_context->close_frame_payload;
	switch_size_t payload_length =
			stream_context->close_frame_payload_length_bytes;
	char close_text[MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES + 1];

	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Close-Initiator",
			stream_context->peer_close_received ? "peer" : "module");
	if (!stream_context->peer_close_received || payload_length < 2) {
		return;
	}

	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Close-Code",
			"%u",
			(unsigned int) (((unsigned int) payload[0] << 8) | payload[1]));
	if (payload_length > 2) {
		memcpy(close_text, payload + 2, payload_length - 2);
		close_text[payload_length - 2] = '\0';
		switch_event_add_header_string(
				event,
				SWITCH_STACK_BOTTOM,
				"WebSocket-Stream-Close-Text",
				close_text);
	}
}

static void worker_fire_transport_event(
		mws_stream_t *stream_context,
		mws_transport_state_t state,
		CURLcode curl_result,
		CURLMcode multi_result,
		const char *error_message)
{
	mws_stream_statistics_t statistics;
	switch_event_t *event = NULL;
	switch_status_t event_status;

	snapshot_stream_statistics(stream_context, &statistics);

	if (switch_event_create_subclass(
			&event,
			SWITCH_EVENT_CUSTOM,
			MWS_TRANSPORT_EVENT_SUBCLASS) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	add_stream_channel_event_data(event, stream_context);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-ID",
			stream_context->stream_id);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-State",
			transport_state_name(state));
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-State",
			playback_state_name(statistics.playback_state));
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Curl-Code",
			"%d",
			(int) curl_result);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Multi-Code",
			"%d",
			(int) multi_result);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Curl-Error",
			zstr(error_message) ? "unknown" : error_message);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Attempt",
			"%u",
			stream_context->consecutive_reconnect_attempts);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Attempts-Total",
			"%" SWITCH_UINT64_T_FMT,
			stream_context->reconnect_attempt_count);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Attempt-Limit",
			"%d",
			stream_context->reconnect_attempt_limit);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Delay-Ms",
			"%u",
			stream_context->consecutive_reconnect_attempts ?
			calculate_reconnect_delay_ms(stream_context) : 0);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Drop-Events",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_drop_events);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Dropped-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_dropped_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_source_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Received-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_received_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Buffered-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_buffered_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Mixed-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_mixed_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Peak-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_peak_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_underflow_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Invalid-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_invalid_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callbacks);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Requested-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_requested_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Intervals",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_intervals);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Interval-Total-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_interval_total_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Max-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_max_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Deferred-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_deferred_callbacks);

	if (state == MWS_TRANSPORT_CLOSED) {
		add_close_event_headers(event, stream_context);
	}
	add_playback_rate_event_headers(event, &statistics);
	add_playback_timing_event_headers(event, &statistics);
	event_status = switch_event_fire(&event);
	if (event_status == SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(globals.worker_mutex);
		globals.transport_event_count++;
		switch_mutex_unlock(globals.worker_mutex);
	}
}

static void worker_fire_statistics_event(mws_stream_t *stream_context)
{
	mws_stream_statistics_t statistics;
	switch_event_t *event = NULL;
	const char *channel_to_websocket;
	const char *websocket_to_channel;
	switch_status_t event_status;

	snapshot_stream_statistics(stream_context, &statistics);
	channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);

	if (switch_event_create_subclass(
			&event,
			SWITCH_EVENT_CUSTOM,
			MWS_STATISTICS_EVENT_SUBCLASS) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"Unique-ID",
			stream_context->session_uuid);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-ID",
			stream_context->stream_id);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-State",
			transport_state_name(statistics.transport_state));
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-State",
			playback_state_name(statistics.playback_state));
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Target-Rate-Hz",
			"%d",
			stream_context->target_sample_rate);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Channel-To-WebSocket",
			channel_to_websocket ? channel_to_websocket : "unknown");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-WebSocket-To-Channel",
			websocket_to_channel ? websocket_to_channel : "unknown");
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.captured_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.captured_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Fallback-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.fallback_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Peak-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_peak_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Drop-Events",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_drop_events);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Dropped-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_dropped_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Received-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_received_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_source_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Buffered-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_buffered_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Mixed-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_mixed_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_underflow_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Invalid-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_invalid_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callbacks);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Requested-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_requested_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Intervals",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_intervals);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Interval-Total-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_interval_total_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Max-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_max_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Deferred-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_deferred_callbacks);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Attempt",
			"%u",
			statistics.consecutive_reconnect_attempts);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Attempts-Total",
			"%" SWITCH_UINT64_T_FMT,
			statistics.reconnect_attempt_count);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Reconnect-Successes",
			"%" SWITCH_UINT64_T_FMT,
			statistics.reconnect_success_count);

	add_playback_rate_event_headers(event, &statistics);
	add_playback_timing_event_headers(event, &statistics);
	event_status = switch_event_fire(&event);
	if (event_status == SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(globals.worker_mutex);
		globals.statistics_event_count++;
		switch_mutex_unlock(globals.worker_mutex);
	}
}

static void worker_fire_playback_overflow_event(
		mws_stream_t *stream_context,
		uint64_t dropped_samples)
{
	mws_stream_statistics_t statistics;
	switch_core_session_t *session;
	switch_event_t *event = NULL;
	const char *channel_to_websocket;
	const char *websocket_to_channel;
	switch_bool_t channel_data_added = SWITCH_FALSE;
	switch_status_t event_status;

	snapshot_stream_statistics(stream_context, &statistics);
	channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);

	if (switch_event_create_subclass(
			&event,
			SWITCH_EVENT_CUSTOM,
			MWS_ERROR_EVENT_SUBCLASS) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	session = switch_core_session_locate(stream_context->session_uuid);
	if (session) {
		switch_channel_event_set_data(
				switch_core_session_get_channel(session),
				event);
		channel_data_added = SWITCH_TRUE;
		switch_core_session_rwunlock(session);
	}

	if (!channel_data_added) {
		switch_event_add_header_string(
				event,
				SWITCH_STACK_BOTTOM,
				"Unique-ID",
				stream_context->session_uuid);
	}
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-ID",
			stream_context->stream_id);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Error",
			MWS_PLAYBACK_BUFFER_OVERFLOW_ERROR);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Error-Message",
			"Playback buffer full; oldest audio discarded");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-URL",
			stream_context->url);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-State",
			transport_state_name(statistics.transport_state));
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-State",
			playback_state_name(statistics.playback_state));
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Target-Rate-Hz",
			"%d",
			stream_context->target_sample_rate);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Channel-To-WebSocket",
			channel_to_websocket ? channel_to_websocket : "unknown");
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-WebSocket-To-Channel",
			websocket_to_channel ? websocket_to_channel : "unknown");
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Dropped-Samples-This-Event",
			"%" SWITCH_UINT64_T_FMT,
			dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Source-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_source_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Peak-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_peak_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Received-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_received_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Buffered-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_buffered_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Mixed-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_mixed_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Dropped-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_dropped_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Underflow-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_underflow_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callbacks);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Requested-Samples",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_requested_samples);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Intervals",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_intervals);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Interval-Total-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_interval_total_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Callback-Max-Interval-Us",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_max_interval_us);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Deferred-Callbacks",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_deferred_callbacks);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Playback-Invalid-Frames",
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_invalid_frames);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Queued-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.captured_queued_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Capacity-Bytes",
			"%" SWITCH_SIZE_T_FMT,
			statistics.captured_capacity_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Drop-Events",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_drop_events);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Captured-Dropped-Bytes",
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_dropped_bytes);
	switch_event_add_header(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Curl-Code",
			"%d",
			(int) stream_context->curl_result);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Curl-Error",
			zstr(stream_context->curl_error) ? "none" :
			stream_context->curl_error);

	add_playback_rate_event_headers(event, &statistics);
	add_playback_timing_event_headers(event, &statistics);
	event_status = switch_event_fire(&event);
	if (event_status == SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(globals.worker_mutex);
		globals.error_event_count++;
		switch_mutex_unlock(globals.worker_mutex);
	}
}

static void worker_mark_transport_open(mws_stream_t *stream_context)
{
	switch_bool_t reconnected;

	if (stream_context->transport_state != MWS_TRANSPORT_CONNECTING) {
		return;
	}

	reconnected = stream_context->consecutive_reconnect_attempts > 0;
	worker_set_transport_state(stream_context, MWS_TRANSPORT_OPEN);
	if (reconnected) {
		switch_mutex_lock(globals.worker_mutex);
		stream_context->reconnect_success_count++;
		globals.reconnect_success_count++;
		switch_mutex_unlock(globals.worker_mutex);
	}
	worker_fire_transport_event(
			stream_context,
			MWS_TRANSPORT_OPEN,
			CURLE_OK,
			CURLM_OK,
			curl_easy_strerror(CURLE_OK));
	if (reconnected) {
		switch_mutex_lock(globals.worker_mutex);
		stream_context->consecutive_reconnect_attempts = 0;
		stream_context->next_reconnect_at = 0;
		switch_mutex_unlock(globals.worker_mutex);
	}
	if (stream_context->statistics_interval_ms > 0) {
		stream_context->next_statistics_event_at = switch_time_now() +
				(switch_time_t) stream_context->statistics_interval_ms * 1000;
	}

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_NOTICE,
			"WebSocket connection %s: uuid=%s stream_id=%s\n",
			reconnected ? "reconnected" : "open",
			stream_context->session_uuid,
			stream_context->stream_id);
}

static void convert_native_pcm_to_little_endian(
		uint8_t *data,
		switch_size_t data_length)
{
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
	switch_swap_linear(
			(int16_t *) data,
			(int) (data_length / sizeof(int16_t)));
#else
	(void) data;
	(void) data_length;
#endif
}

static void worker_begin_playback_buffer_maintenance(
		mws_stream_t *stream_context)
{
	mws_playback_audio_buffer_request_maintenance(
			&stream_context->playback_audio_buffer);
	while (mws_playback_audio_buffer_consumer_is_active(
			&stream_context->playback_audio_buffer)) {
		switch_yield(1);
	}
}

static void worker_end_playback_buffer_maintenance(
		mws_stream_t *stream_context)
{
	mws_playback_audio_buffer_finish_maintenance(
			&stream_context->playback_audio_buffer);
}

static uint64_t worker_reset_playback_buffer(
		mws_stream_t *stream_context,
		uint32_t capacity_bytes)
{
	uint32_t discarded_bytes;

	worker_begin_playback_buffer_maintenance(stream_context);
	discarded_bytes =
			mws_playback_audio_buffer_reset_during_maintenance(
					&stream_context->playback_audio_buffer,
					capacity_bytes);
	worker_end_playback_buffer_maintenance(stream_context);

	return discarded_bytes / sizeof(int16_t);
}

static void update_playback_peak_queued_bytes(
		mws_stream_t *stream_context,
		uint32_t queued_bytes)
{
	uint32_t current_peak = atomic_load_explicit(
			&stream_context->playback_peak_queued_bytes,
			memory_order_relaxed);

	while (queued_bytes > current_peak &&
			!atomic_compare_exchange_weak_explicit(
				&stream_context->playback_peak_queued_bytes,
				&current_peak,
				queued_bytes,
				memory_order_relaxed,
				memory_order_relaxed)) {
	}
}

static switch_status_t worker_apply_pending_playback_format(
		mws_stream_t *stream_context)
{
	uint32_t playback_sample_rate;
	uint32_t capacity_bytes;
	uint64_t discarded_samples;

	if (!atomic_load_explicit(
			&stream_context->playback_format_reset_requested,
			memory_order_acquire)) {
		return SWITCH_STATUS_SUCCESS;
	}

	playback_sample_rate = atomic_load_explicit(
			&stream_context->playback_sample_rate,
			memory_order_acquire);
	capacity_bytes = (uint32_t) calculate_pcm_capacity_bytes(
			playback_sample_rate,
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	if (!capacity_bytes || capacity_bytes >=
			stream_context->playback_audio_buffer.allocation_bytes) {
		return SWITCH_STATUS_FALSE;
	}

	discarded_samples = worker_reset_playback_buffer(
			stream_context,
			capacity_bytes);
	atomic_fetch_add_explicit(
			&stream_context->playback_dropped_sample_count,
			discarded_samples,
			memory_order_relaxed);
	atomic_store_explicit(
			&stream_context->playback_state,
			MWS_PLAYBACK_BUFFERING,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_overflow_active,
			0,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_format_reset_requested,
			0,
			memory_order_release);

	return SWITCH_STATUS_SUCCESS;
}

static uint64_t playback_queued_duration_us(
		mws_stream_t *stream_context,
		uint32_t playback_sample_rate)
{
	uint64_t source_samples = mws_source_audio_buffer_queued_bytes(
			&stream_context->source_audio_buffer) / sizeof(int16_t);
	uint64_t playback_samples = mws_playback_audio_buffer_queued_bytes(
			&stream_context->playback_audio_buffer) / sizeof(int16_t);

	return mws_playback_queued_duration_us(
			source_samples,
			(uint32_t) stream_context->target_sample_rate,
			playback_samples,
			playback_sample_rate);
}

static switch_status_t worker_buffer_playback_output(
		mws_stream_t *stream_context,
		const uint8_t *playback_pcm,
		switch_size_t playback_length_bytes,
		uint32_t format_generation,
		uint32_t playback_sample_rate)
{
	uint32_t playback_capacity_bytes;
	uint32_t queued_bytes;
	uint64_t overflow_dropped_samples = 0;
	switch_bool_t fire_overflow_event = SWITCH_FALSE;
	switch_bool_t playback_buffer_maintenance_started = SWITCH_FALSE;

	if (!playback_length_bytes) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (atomic_load_explicit(
			&stream_context->playback_format_reset_requested,
			memory_order_acquire) ||
			atomic_load_explicit(
				&stream_context->playback_format_generation,
				memory_order_acquire) != format_generation ||
			atomic_load_explicit(
				&stream_context->playback_sample_rate,
				memory_order_acquire) != playback_sample_rate) {
		atomic_fetch_add_explicit(
				&stream_context->playback_dropped_sample_count,
				playback_length_bytes / sizeof(int16_t),
				memory_order_relaxed);
		return SWITCH_STATUS_SUCCESS;
	}

	playback_capacity_bytes = mws_playback_audio_buffer_capacity_bytes(
			&stream_context->playback_audio_buffer);
	queued_bytes = mws_playback_audio_buffer_queued_bytes(
			&stream_context->playback_audio_buffer);
	if (playback_length_bytes > playback_capacity_bytes ||
			playback_length_bytes > playback_capacity_bytes - queued_bytes) {
		worker_begin_playback_buffer_maintenance(stream_context);
		playback_buffer_maintenance_started = SWITCH_TRUE;
		queued_bytes = mws_playback_audio_buffer_queued_bytes(
				&stream_context->playback_audio_buffer);

		if (playback_length_bytes > playback_capacity_bytes) {
		switch_size_t bytes_to_skip = playback_length_bytes -
				playback_capacity_bytes;

		bytes_to_skip -= bytes_to_skip % sizeof(int16_t);
		overflow_dropped_samples +=
				mws_playback_audio_buffer_discard_all_during_maintenance(
						&stream_context->playback_audio_buffer) /
				sizeof(int16_t);
		overflow_dropped_samples += bytes_to_skip / sizeof(int16_t);
		playback_pcm += bytes_to_skip;
		playback_length_bytes -= bytes_to_skip;
		} else if (playback_length_bytes >
				playback_capacity_bytes - queued_bytes) {
			uint32_t bytes_to_discard =
					queued_bytes + (uint32_t) playback_length_bytes -
					playback_capacity_bytes;

			overflow_dropped_samples +=
					mws_playback_audio_buffer_discard_oldest_during_maintenance(
							&stream_context->playback_audio_buffer,
							bytes_to_discard) /
					sizeof(int16_t);
		}
	}

	mws_playback_audio_buffer_write(
			&stream_context->playback_audio_buffer,
			playback_pcm,
			(uint32_t) playback_length_bytes);
	mws_playback_timing_note_playback_buffer_write(
			&stream_context->playback_timing,
			(uint64_t) switch_mono_micro_time_now());
	if (playback_buffer_maintenance_started) {
		worker_end_playback_buffer_maintenance(stream_context);
	}
	if (overflow_dropped_samples) {
		atomic_fetch_add_explicit(
				&stream_context->playback_dropped_sample_count,
				overflow_dropped_samples,
				memory_order_relaxed);
	}
	queued_bytes = mws_playback_audio_buffer_queued_bytes(
			&stream_context->playback_audio_buffer);
	update_playback_peak_queued_bytes(stream_context, queued_bytes);
	atomic_fetch_add_explicit(
			&stream_context->playback_buffered_sample_count,
			playback_length_bytes / sizeof(int16_t),
			memory_order_relaxed);
	if (overflow_dropped_samples &&
			!atomic_exchange_explicit(
				&stream_context->playback_overflow_active,
				1,
				memory_order_acq_rel)) {
		fire_overflow_event = SWITCH_TRUE;
	}

	if (fire_overflow_event) {
		worker_fire_playback_overflow_event(
				stream_context,
				overflow_dropped_samples);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t worker_process_playback_source(
		mws_stream_t *stream_context)
{
	mws_playback_audio_resampler_output_t resampled_audio;
	mws_playback_audio_resampler_result_t resampler_result;
	uint32_t playback_capacity_bytes;
	uint32_t playback_queued_bytes;
	uint32_t playback_free_bytes;
	uint32_t playback_target_bytes;
	uint32_t playback_sample_rate;
	uint32_t format_generation;

	if (stream_context->transport_state != MWS_TRANSPORT_OPEN) {
		return SWITCH_STATUS_SUCCESS;
	}
	mws_playback_timing_note_worker_visit(
			&stream_context->playback_timing,
			(uint64_t) switch_mono_micro_time_now());

	if (worker_apply_pending_playback_format(stream_context) !=
			SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	playback_sample_rate = atomic_load_explicit(
			&stream_context->playback_sample_rate,
			memory_order_acquire);
	format_generation = atomic_load_explicit(
			&stream_context->playback_format_generation,
			memory_order_acquire);
	if (mws_playback_audio_resampler_update(
			&stream_context->playback_audio_resampler,
			format_generation,
			playback_sample_rate,
			atomic_load_explicit(
					&stream_context->playback_state,
					memory_order_acquire) == MWS_PLAYBACK_PLAYING,
			mws_source_audio_buffer_queued_bytes(
					&stream_context->source_audio_buffer) /
					sizeof(int16_t),
			mws_playback_audio_buffer_queued_bytes(
					&stream_context->playback_audio_buffer) /
					sizeof(int16_t),
			switch_mono_micro_time_now()) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	if (!mws_source_audio_buffer_queued_bytes(
			&stream_context->source_audio_buffer)) {
		return SWITCH_STATUS_SUCCESS;
	}

	playback_capacity_bytes = mws_playback_audio_buffer_capacity_bytes(
			&stream_context->playback_audio_buffer);
	playback_queued_bytes = mws_playback_audio_buffer_queued_bytes(
			&stream_context->playback_audio_buffer);
	playback_target_bytes = (uint32_t) calculate_pcm_capacity_bytes(
			playback_sample_rate,
			1,
			MWS_PLAYBACK_START_BUFFER_MS);
	if (!playback_target_bytes ||
			playback_target_bytes > playback_capacity_bytes) {
		return SWITCH_STATUS_FALSE;
	}
	if (playback_queued_bytes >= playback_target_bytes) {
		return SWITCH_STATUS_SUCCESS;
	}
	playback_free_bytes = playback_target_bytes - playback_queued_bytes;
	playback_free_bytes -= playback_free_bytes % sizeof(int16_t);
	if (!playback_free_bytes) {
		return SWITCH_STATUS_SUCCESS;
	}

	resampler_result = mws_playback_audio_resampler_convert(
			&stream_context->playback_audio_resampler,
			&stream_context->source_audio_buffer,
			playback_free_bytes,
			&resampled_audio);
	if (resampler_result == MWS_PLAYBACK_AUDIO_RESAMPLER_ERROR) {
		return SWITCH_STATUS_FALSE;
	}
	if (resampler_result == MWS_PLAYBACK_AUDIO_RESAMPLER_EMPTY) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (resampled_audio.length_bytes &&
			worker_buffer_playback_output(
				stream_context,
				resampled_audio.data,
				resampled_audio.length_bytes,
				format_generation,
				playback_sample_rate) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	if (resampled_audio.consumed_source_bytes) {
		mws_source_audio_buffer_consume(
				&stream_context->source_audio_buffer,
				resampled_audio.consumed_source_bytes);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t worker_accept_inbound_pcm(
		mws_stream_t *stream_context,
		const uint8_t *data,
		switch_size_t data_length)
{
	mws_websocket_audio_input_write_result_t write_result;
	mws_websocket_audio_input_result_t result =
			mws_websocket_audio_input_write(
					&stream_context->websocket_audio_input,
					&stream_context->source_audio_buffer,
					data,
					data_length,
					&write_result);

	if (result == MWS_WEBSOCKET_AUDIO_INPUT_FULL) {
		stream_context->receive_pause_required_bytes =
				write_result.required_free_bytes;
		return SWITCH_STATUS_BREAK;
	}
	if (result != MWS_WEBSOCKET_AUDIO_INPUT_ACCEPTED) {
		return SWITCH_STATUS_FALSE;
	}
	atomic_fetch_add_explicit(
			&stream_context->playback_received_sample_count,
			write_result.accepted_sample_count,
			memory_order_relaxed);
	if (write_result.accepted_sample_count) {
		mws_playback_timing_note_websocket_audio(
				&stream_context->playback_timing,
				(uint64_t) switch_mono_micro_time_now());
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t worker_queue_text_response(
		mws_stream_t *stream_context,
		const char *response)
{
	char *queue_slot;
	switch_size_t response_length;

	if (zstr(response) || stream_context->text_response_count >=
			MWS_TEXT_RESPONSE_QUEUE_CAPACITY) {
		stream_context->dropped_text_response_count++;
		return SWITCH_STATUS_FALSE;
	}

	response_length = strlen(response);
	if (response_length >= MWS_MAXIMUM_INTERNAL_RESPONSE_BYTES) {
		stream_context->dropped_text_response_count++;
		return SWITCH_STATUS_FALSE;
	}

	queue_slot = stream_context->text_response_queue
			[stream_context->text_response_write_index];
	memcpy(queue_slot, response, response_length + 1);
	stream_context->text_response_lengths
			[stream_context->text_response_write_index] = response_length;
	stream_context->text_response_write_index =
			(stream_context->text_response_write_index + 1) %
			MWS_TEXT_RESPONSE_QUEUE_CAPACITY;
	stream_context->text_response_count++;

	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t api_command_worker_accepts_commands(void)
{
	switch_bool_t accepts_commands;

	switch_mutex_lock(globals.api_command_worker_mutex);
	accepts_commands = globals.api_command_worker_running &&
			!globals.api_command_worker_stopping;
	switch_mutex_unlock(globals.api_command_worker_mutex);

	return accepts_commands;
}

static void remove_queued_api_command_task_locked(
		mws_api_command_task_t *task)
{
	mws_api_command_task_t **current = &globals.queued_api_command_tasks;

	while (*current && *current != task) {
		current = &(*current)->queued_next;
	}
	switch_assert(*current == task);
	*current = task->queued_next;
	task->queued_next = NULL;
}

static switch_bool_t api_command_task_popped(mws_api_command_task_t *task)
{
	switch_bool_t stopping;

	switch_mutex_lock(globals.api_command_worker_mutex);
	if (task && task != &globals.api_command_stop_task) {
		switch_assert(globals.api_command_tasks_queued > 0);
		remove_queued_api_command_task_locked(task);
		globals.api_command_tasks_queued--;
	}
	stopping = globals.api_command_worker_stopping;
	switch_mutex_unlock(globals.api_command_worker_mutex);

	return stopping;
}

static void worker_enqueue_api_command(
		mws_stream_t *stream_context,
		const char *command,
		switch_size_t command_length)
{
	mws_api_command_task_t *task;
	switch_status_t status;

	if (!command_length) {
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR empty FreeSWITCH API command\n");
		return;
	}

	if (memchr(command, '\0', command_length)) {
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR FreeSWITCH API command contains a NUL byte\n");
		return;
	}

	if (!api_command_worker_accepts_commands()) {
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR FreeSWITCH API command executor is stopping\n");
		return;
	}

	task = malloc(sizeof(*task));
	if (!task) {
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR could not allocate FreeSWITCH API command\n");
		return;
	}
	copy_stream_identity(&task->identity, stream_context);
	memcpy(task->command, command, command_length);
	task->command[command_length] = '\0';
	task->queued_next = NULL;

	switch_mutex_lock(stream_context->api_command_mutex);
	if (stream_context->api_command_active) {
		switch_mutex_unlock(stream_context->api_command_mutex);
		switch_safe_free(task);
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR another FreeSWITCH API command is already running\n");
		return;
	}

	stream_context->api_command_active = SWITCH_TRUE;
	stream_context->api_command_received_count++;
	switch_mutex_unlock(stream_context->api_command_mutex);

	switch_mutex_lock(globals.api_command_worker_mutex);
	if (globals.api_command_worker_running &&
			!globals.api_command_worker_stopping &&
			globals.api_command_tasks_queued <
					MWS_API_COMMAND_QUEUE_CAPACITY) {
		task->queued_next = globals.queued_api_command_tasks;
		globals.queued_api_command_tasks = task;
		globals.api_command_tasks_queued++;
		status = switch_queue_trypush(
				globals.api_command_tasks,
				task);
		if (status != SWITCH_STATUS_SUCCESS) {
			remove_queued_api_command_task_locked(task);
			globals.api_command_tasks_queued--;
		}
	} else {
		status = SWITCH_STATUS_FALSE;
	}
	switch_mutex_unlock(globals.api_command_worker_mutex);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(stream_context->api_command_mutex);
		stream_context->api_command_active = SWITCH_FALSE;
		switch_mutex_unlock(stream_context->api_command_mutex);
		switch_safe_free(task);
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR FreeSWITCH API command executor is unavailable\n");
	}
}

static uint64_t worker_clear_websocket_audio(mws_stream_t *stream_context)
{
	uint64_t discarded_sample_count;
	uint64_t source_discarded_sample_count;
	uint64_t source_discarded_at_playback_rate;
	uint32_t capacity_bytes;
	uint32_t playback_sample_rate;

	mws_playback_audio_resampler_reset(
			&stream_context->playback_audio_resampler);
	mws_websocket_audio_input_clear(
			&stream_context->websocket_audio_input);
	source_discarded_sample_count =
			mws_source_audio_buffer_discard_all(
					&stream_context->source_audio_buffer) /
			sizeof(int16_t);
	atomic_fetch_add_explicit(
			&stream_context->playback_source_dropped_sample_count,
			source_discarded_sample_count,
			memory_order_relaxed);

	playback_sample_rate = atomic_load_explicit(
			&stream_context->playback_sample_rate,
			memory_order_acquire);
	capacity_bytes = (uint32_t) calculate_pcm_capacity_bytes(
			playback_sample_rate,
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	discarded_sample_count = worker_reset_playback_buffer(
			stream_context,
			capacity_bytes);
	atomic_fetch_add_explicit(
			&stream_context->playback_dropped_sample_count,
			discarded_sample_count,
			memory_order_relaxed);
	atomic_store_explicit(
			&stream_context->playback_state,
			MWS_PLAYBACK_BUFFERING,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_overflow_active,
			0,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_format_reset_requested,
			0,
			memory_order_release);

	source_discarded_at_playback_rate =
			(source_discarded_sample_count * playback_sample_rate +
			 stream_context->target_sample_rate / 2) /
			stream_context->target_sample_rate;
	discarded_sample_count += source_discarded_at_playback_rate;
	stream_context->clear_command_count++;
	stream_context->clear_discarded_sample_count += discarded_sample_count;

	return discarded_sample_count;
}

static void worker_reject_json_message(
		mws_stream_t *stream_context,
		const char *error,
		const char *message)
{
	char response[MWS_MAXIMUM_INTERNAL_RESPONSE_BYTES];

	atomic_fetch_add_explicit(
			&stream_context->rejected_message_count,
			1,
			memory_order_relaxed);
	switch_snprintf(
			response,
			sizeof(response),
			"{\"event\":\"error\",\"error\":\"%s\",\"message\":\"%s\"}",
			error,
			message);
	(void) worker_queue_text_response(stream_context, response);
}

static switch_bool_t message_event_name_is_valid(const char *name)
{
	const char *character;

	if (zstr(name) ||
			strlen(name) > MWS_MAXIMUM_MESSAGE_EVENT_NAME_BYTES) {
		return SWITCH_FALSE;
	}
	for (character = name; *character; character++) {
		if (!isalnum((unsigned char) *character) &&
				*character != '_' && *character != '-' &&
				*character != '.' && *character != ':') {
			return SWITCH_FALSE;
		}
	}

	return SWITCH_TRUE;
}

/* Names the module uses itself, or reserves for future control messages. */
static switch_bool_t message_event_name_is_reserved(const char *name)
{
	static const char *reserved_names[] = {
		"start", "stop", "error", "mark", NULL
	};
	const char **reserved;

	for (reserved = reserved_names; *reserved; reserved++) {
		if (!strcasecmp(name, *reserved)) {
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

static switch_status_t worker_fire_message_event(
		mws_stream_t *stream_context,
		const char *event_name,
		const char *body)
{
	switch_event_t *event = NULL;
	switch_status_t status;

	if (switch_event_create_subclass(
			&event,
			SWITCH_EVENT_CUSTOM,
			MWS_MESSAGE_EVENT_SUBCLASS) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	add_stream_channel_event_data(event, stream_context);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-ID",
			stream_context->stream_id);
	switch_event_add_header_string(
			event,
			SWITCH_STACK_BOTTOM,
			"WebSocket-Stream-Message-Event",
			event_name);
	switch_event_add_body(event, "%s", body);

	status = switch_event_fire(&event);
	if (status == SWITCH_STATUS_SUCCESS) {
		atomic_fetch_add_explicit(
				&stream_context->message_event_count,
				1,
				memory_order_relaxed);
		switch_mutex_lock(globals.worker_mutex);
		globals.message_event_count++;
		switch_mutex_unlock(globals.worker_mutex);
	} else if (event) {
		switch_event_destroy(&event);
	}

	return status;
}

/*
 * A JSON object with a string "event" member. "clear" discards queued
 * WebSocket audio on this stream; any other name is fired as a
 * mod_websocket_stream::message event carrying the original JSON as its body.
 */
static void worker_accept_json_message(
		mws_stream_t *stream_context,
		const char *message,
		switch_size_t message_length)
{
	cJSON *root = NULL;
	const char *event_name;

	if (memchr(message, '\0', message_length)) {
		worker_reject_json_message(
				stream_context,
				"invalid_message",
				"message contains a NUL byte");
		return;
	}

	root = cJSON_Parse(message);
	if (!root || !cJSON_IsObject(root)) {
		worker_reject_json_message(
				stream_context,
				"invalid_message",
				"message is not a JSON object");
		goto done;
	}

	event_name = cJSON_GetObjectCstr(root, "event");
	if (!message_event_name_is_valid(event_name)) {
		worker_reject_json_message(
				stream_context,
				"invalid_event",
				"event must be a string of up to 128 letters, digits, "
				"or _ - . : characters");
		goto done;
	}

	if (!strcasecmp(event_name, "clear")) {
		(void) worker_clear_websocket_audio(stream_context);
		goto done;
	}

	if (message_event_name_is_reserved(event_name)) {
		worker_reject_json_message(
				stream_context,
				"reserved_event",
				"event name is reserved by mod_websocket_stream");
		goto done;
	}

	if (worker_fire_message_event(
			stream_context,
			event_name,
			message) != SWITCH_STATUS_SUCCESS) {
		worker_reject_json_message(
				stream_context,
				"event_failed",
				"could not fire FreeSWITCH event");
	}

done:
	if (root) {
		cJSON_Delete(root);
	}
}

static void worker_accept_inbound_text(
		mws_stream_t *stream_context,
		const char *data,
		switch_size_t data_length,
		const struct curl_ws_frame *metadata)
{
	switch_size_t available_bytes;
	switch_size_t index;
	switch_bool_t json_message;

	if (!stream_context->inbound_text_message_active) {
		stream_context->inbound_text_message_active = SWITCH_TRUE;
		stream_context->inbound_text_message_length_bytes = 0;
		stream_context->inbound_text_message_too_large = SWITCH_FALSE;
		stream_context->inbound_text_message_first_byte = '\0';
	}

	for (index = 0; !stream_context->inbound_text_message_first_byte &&
			index < data_length; index++) {
		if (!isspace((unsigned char) data[index])) {
			stream_context->inbound_text_message_first_byte = data[index];
		}
	}

	available_bytes = MWS_MAXIMUM_TEXT_MESSAGE_BYTES -
			stream_context->inbound_text_message_length_bytes;
	if (!stream_context->inbound_text_message_too_large) {
		if (data_length > available_bytes) {
			stream_context->inbound_text_message_too_large = SWITCH_TRUE;
		} else if (data_length) {
			memcpy(
					stream_context->inbound_text_message +
							stream_context->inbound_text_message_length_bytes,
					data,
					data_length);
			stream_context->inbound_text_message_length_bytes += data_length;
		}
	}

	if (metadata->bytesleft || (metadata->flags & CURLWS_CONT)) {
		return;
	}

	/* API command names never begin with '{', so JSON is unambiguous. */
	json_message = stream_context->inbound_text_message_first_byte == '{';
	if (stream_context->inbound_text_message_too_large) {
		if (json_message) {
			worker_reject_json_message(
					stream_context,
					"message_too_large",
					"JSON messages may not exceed 4096 bytes");
		} else {
			atomic_fetch_add_explicit(
					&stream_context->rejected_api_command_count,
					1,
					memory_order_relaxed);
			(void) worker_queue_text_response(
					stream_context,
					"-ERR FreeSWITCH API commands may not exceed 4096 bytes\n");
		}
	} else if (json_message) {
		stream_context->inbound_text_message
				[stream_context->inbound_text_message_length_bytes] = '\0';
		worker_accept_json_message(
				stream_context,
				stream_context->inbound_text_message,
				stream_context->inbound_text_message_length_bytes);
	} else if (!stream_context->api_commands_enabled) {
		atomic_fetch_add_explicit(
				&stream_context->rejected_api_command_count,
				1,
				memory_order_relaxed);
		(void) worker_queue_text_response(
				stream_context,
				"-ERR FreeSWITCH API commands are disabled for this stream\n");
	} else {
		stream_context->inbound_text_message
				[stream_context->inbound_text_message_length_bytes] = '\0';
		worker_enqueue_api_command(
				stream_context,
				stream_context->inbound_text_message,
				stream_context->inbound_text_message_length_bytes);
	}

	stream_context->inbound_text_message_active = SWITCH_FALSE;
	stream_context->inbound_text_message_length_bytes = 0;
	stream_context->inbound_text_message_too_large = SWITCH_FALSE;
}

static void api_command_abandon_result(mws_stream_t *stream_context)
{
	switch_mutex_lock(stream_context->api_command_mutex);
	switch_safe_free(stream_context->api_command_result);
	stream_context->api_command_result_length_bytes = 0;
	stream_context->api_command_result_offset_bytes = 0;
	stream_context->api_command_result_frame_started = SWITCH_FALSE;
	stream_context->api_command_result_ready = SWITCH_FALSE;
	stream_context->api_command_active = SWITCH_FALSE;
	switch_mutex_unlock(stream_context->api_command_mutex);
}

static mws_stream_t *borrow_registered_stream(
		switch_core_session_t *session,
		const mws_stream_identity_t *identity)
{
	mws_stream_t *current;
	mws_stream_t *match = NULL;
	const char *session_uuid;

	if (!session || !identity) {
		return NULL;
	}

	session_uuid = switch_core_session_get_uuid(session);
	if (zstr(session_uuid) ||
			strcmp(session_uuid, identity->session_uuid)) {
		return NULL;
	}

	switch_mutex_lock(globals.streams_mutex);
	for (current = globals.streams; current; current = current->next) {
		if (!strcmp(current->session_uuid, identity->session_uuid) &&
				!strcmp(current->stream_id, identity->stream_id)) {
			current->resource_borrow_count++;
			match = current;
			break;
		}
	}
	switch_mutex_unlock(globals.streams_mutex);

	return match;
}

static void *SWITCH_THREAD_FUNC api_command_worker(
		switch_thread_t *thread,
		void *data)
{
	(void) thread;
	(void) data;

	for (;;) {
		mws_api_command_task_t *task = NULL;
		mws_stream_t *stream_context = NULL;
		switch_core_session_t *session = NULL;
		switch_stream_handle_t api_stream = { 0 };
		switch_status_t api_status = SWITCH_STATUS_FALSE;
		mws_freeswitch_command_t command;
		bool command_present;
		switch_bool_t discard_task;

		if (switch_queue_pop(
					globals.api_command_tasks,
					(void **) &task) != SWITCH_STATUS_SUCCESS) {
			break;
		}
		discard_task = api_command_task_popped(task);
		if (task == &globals.api_command_stop_task) {
			break;
		}
		if (!task || discard_task) {
			switch_safe_free(task);
			continue;
		}

		session = switch_core_session_locate(task->identity.session_uuid);
		if (!session) {
			switch_safe_free(task);
			continue;
		}
		stream_context = borrow_registered_stream(
				session,
				&task->identity);
		if (!stream_context) {
			switch_core_session_rwunlock(session);
			switch_safe_free(task);
			continue;
		}

		switch_mutex_lock(stream_context->api_command_mutex);
		if (!stream_context->api_command_active) {
			switch_mutex_unlock(stream_context->api_command_mutex);
			release_stream_resource_borrow(stream_context);
			switch_core_session_rwunlock(session);
			switch_safe_free(task);
			continue;
		}
		switch_mutex_unlock(stream_context->api_command_mutex);

		command_present = mws_freeswitch_command_parse(
				task->command,
				&command);

		if (!mws_bounded_stream_initialize(
				&api_stream,
				stream_context->maximum_api_response_bytes)) {
			api_status = SWITCH_STATUS_MEMERR;
			if (mws_bounded_stream_initialize(
					&api_stream,
					sizeof(MWS_API_RESPONSE_ALLOCATION_ERROR) - 1)) {
				api_stream.write_function(
						&api_stream,
						MWS_API_RESPONSE_ALLOCATION_ERROR);
			}
		} else if (command_present &&
				mws_freeswitch_command_would_stop_module_or_server(
					&command)) {
			api_stream.write_function(
					&api_stream,
					"-ERR command cannot stop FreeSWITCH or "
					"mod_websocket_stream from its WebSocket worker\n");
			atomic_fetch_add_explicit(
					&stream_context->rejected_api_command_count,
					1,
					memory_order_relaxed);
		} else if (command_present) {
			api_status = switch_api_execute(
					command.name,
					command.arguments,
					NULL,
					&api_stream);
		} else {
			api_stream.write_function(
					&api_stream,
					"-ERR empty FreeSWITCH API command\n");
		}

		if (api_stream.data && mws_bounded_stream_overflowed(&api_stream)) {
			mws_bounded_stream_replace(
					&api_stream,
					"-ERR FreeSWITCH API response is too large\n");
			api_status = SWITCH_STATUS_FALSE;
		}
		if (api_stream.data && !*((char *) api_stream.data)) {
			api_stream.write_function(
					&api_stream,
					api_status == SWITCH_STATUS_SUCCESS ?
							"+OK\n" : "-ERR FreeSWITCH API command failed\n");
		}

		switch_mutex_lock(stream_context->api_command_mutex);
		if (!stream_context->api_command_active || !api_stream.data) {
			if (stream_context->api_command_active && !api_stream.data) {
				stream_context->api_command_active = SWITCH_FALSE;
				stream_context->api_command_executed_count++;
				stream_context->api_command_failed_count++;
			}
			switch_mutex_unlock(stream_context->api_command_mutex);
			release_stream_resource_borrow(stream_context);
			switch_core_session_rwunlock(session);
			switch_safe_free(api_stream.data);
			switch_safe_free(task);
			continue;
		}
		stream_context->api_command_result = (char *) api_stream.data;
		stream_context->api_command_result_length_bytes =
				strlen((char *) api_stream.data);
		stream_context->api_command_executed_count++;
		stream_context->api_command_result_byte_count +=
				stream_context->api_command_result_length_bytes;
		if (api_status != SWITCH_STATUS_SUCCESS) {
			stream_context->api_command_failed_count++;
		}
		stream_context->api_command_result_ready = SWITCH_TRUE;
		switch_mutex_unlock(stream_context->api_command_mutex);
		api_stream.data = NULL;
		release_stream_resource_borrow(stream_context);
		switch_core_session_rwunlock(session);

		switch_safe_free(api_stream.data);
		switch_safe_free(task);
	}

	switch_mutex_lock(globals.api_command_worker_mutex);
	globals.api_command_worker_running = SWITCH_FALSE;
	switch_mutex_unlock(globals.api_command_worker_mutex);

	return NULL;
}

static switch_status_t start_api_command_worker(switch_memory_pool_t *pool)
{
	switch_threadattr_t *thread_attributes;
	switch_status_t status;

	if (switch_mutex_init(
				&globals.api_command_worker_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_queue_create(
				&globals.api_command_tasks,
				MWS_API_COMMAND_QUEUE_STORAGE_CAPACITY,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_threadattr_create(
				&thread_attributes,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_threadattr_detach_set(thread_attributes, 0);
	switch_threadattr_stacksize_set(
			thread_attributes,
			SWITCH_THREAD_STACKSIZE);
	globals.api_command_stop_task.identity.session_uuid[0] = '\0';
	globals.api_command_stop_task.identity.stream_id[0] = '\0';
	globals.api_command_worker_running = SWITCH_TRUE;
	globals.api_command_worker_stopping = SWITCH_FALSE;
	globals.api_command_stop_queued = SWITCH_FALSE;
	globals.api_command_tasks_queued = 0;
	globals.queued_api_command_tasks = NULL;

	status = switch_thread_create(
			&globals.api_command_worker_thread,
			thread_attributes,
			api_command_worker,
			NULL,
			pool);
	if (status != SWITCH_STATUS_SUCCESS) {
		globals.api_command_worker_thread = NULL;
		globals.api_command_worker_running = SWITCH_FALSE;
		globals.api_command_worker_stopping = SWITCH_TRUE;
	}

	return status;
}

static void discard_queued_api_command_tasks(void)
{
	mws_api_command_task_t *task;

	switch_mutex_lock(globals.api_command_worker_mutex);
	task = globals.queued_api_command_tasks;
	globals.queued_api_command_tasks = NULL;
	globals.api_command_tasks_queued = 0;
	switch_mutex_unlock(globals.api_command_worker_mutex);

	while (task) {
		mws_api_command_task_t *next_task = task->queued_next;

		task->queued_next = NULL;
		switch_safe_free(task);
		task = next_task;
	}
}

static switch_status_t stop_api_command_worker(void)
{
	switch_thread_t *worker_thread;
	switch_status_t thread_status = SWITCH_STATUS_SUCCESS;
	switch_status_t status;

	switch_mutex_lock(globals.api_command_worker_mutex);
	worker_thread = globals.api_command_worker_thread;
	if (!worker_thread) {
		switch_mutex_unlock(globals.api_command_worker_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	globals.api_command_worker_stopping = SWITCH_TRUE;
	if (globals.api_command_stop_queued) {
		status = SWITCH_STATUS_SUCCESS;
	} else {
		status = switch_queue_trypush(
				globals.api_command_tasks,
				&globals.api_command_stop_task);
		if (status == SWITCH_STATUS_SUCCESS) {
			globals.api_command_stop_queued = SWITCH_TRUE;
		}
	}
	switch_mutex_unlock(globals.api_command_worker_mutex);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_queue_term(globals.api_command_tasks);
	}

	status = switch_thread_join(&thread_status, worker_thread);
	if (status != SWITCH_STATUS_SUCCESS) {
		return status;
	}
	discard_queued_api_command_tasks();

	switch_mutex_lock(globals.api_command_worker_mutex);
	globals.api_command_worker_thread = NULL;
	globals.api_command_worker_running = SWITCH_FALSE;
	globals.api_command_stop_queued = SWITCH_FALSE;
	globals.api_command_tasks_queued = 0;
	switch_mutex_unlock(globals.api_command_worker_mutex);

	return thread_status;
}

static mws_stream_t *take_pending_detached_cleanups(void)
{
	mws_stream_t *stream_context;

	switch_mutex_lock(globals.cleanup_worker_mutex);
	stream_context = globals.pending_detached_cleanups;
	globals.pending_detached_cleanups = NULL;
	globals.pending_detached_cleanup_count = 0;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	return stream_context;
}

static void finish_pending_detached_cleanups(void)
{
	mws_stream_t *stream_context = take_pending_detached_cleanups();

	while (stream_context) {
		mws_stream_t *next_stream = stream_context->pending_cleanup_next;

		stream_context->pending_cleanup_next = NULL;
		finish_detached_stream_cleanup(stream_context);
		stream_context = next_stream;
	}
}

static mws_stream_t *take_pending_callback_holds(void)
{
	mws_stream_t *stream_context;

	switch_mutex_lock(globals.cleanup_worker_mutex);
	stream_context = globals.pending_callback_holds;
	globals.pending_callback_holds = NULL;
	globals.pending_callback_hold_count = 0;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	return stream_context;
}

static void release_callback_hold(mws_stream_t *stream_context)
{
	mws_stream_t *current;
	switch_memory_pool_t *pool;
	switch_bool_t registered = SWITCH_FALSE;

	switch_mutex_lock(globals.streams_mutex);
	for (current = globals.streams; current; current = current->next) {
		if (current == stream_context) {
			registered = SWITCH_TRUE;
			break;
		}
	}
	if (stream_context->cleanup_holds_stream_slot) {
		switch_assert(stream_context->admission_reserved);
		stream_context->cleanup_holds_stream_slot = SWITCH_FALSE;
		if (!registered) {
			switch_assert(globals.reserved_streams > 0);
			globals.reserved_streams--;
			stream_context->admission_reserved = SWITCH_FALSE;
			stream_context->admission_channel_context = NULL;
		}
	}
	switch_assert(stream_context->resource_borrow_count > 0);
	stream_context->resource_borrow_count--;
	pool = claim_stream_resource_pool_locked(stream_context);
	switch_mutex_unlock(globals.streams_mutex);

	destroy_stream_resource_pool(stream_context, pool);
}

static void release_pending_callback_holds(void)
{
	mws_stream_t *stream_context = take_pending_callback_holds();

	while (stream_context) {
		mws_stream_t *next_stream =
				stream_context->pending_callback_hold_next;

		stream_context->pending_callback_hold_next = NULL;
		release_callback_hold(stream_context);
		stream_context = next_stream;
	}
}

static void queue_cleanup_wake_locked(void)
{
	if (globals.cleanup_worker_running &&
			!globals.cleanup_worker_stopping &&
			!globals.cleanup_wake_queued &&
			switch_queue_trypush(
					globals.cleanup_tasks,
					&globals.cleanup_wake_task) ==
					SWITCH_STATUS_SUCCESS) {
		globals.cleanup_wake_queued = SWITCH_TRUE;
	}
}

static void remove_queued_cleanup_task_locked(mws_cleanup_task_t *task)
{
	mws_cleanup_task_t **current = &globals.queued_cleanup_tasks;

	while (*current && *current != task) {
		current = &(*current)->queued_next;
	}
	switch_assert(*current == task);
	*current = task->queued_next;
	task->queued_next = NULL;
}

static switch_bool_t cleanup_task_popped(mws_cleanup_task_t *task)
{
	switch_bool_t stopping;

	switch_mutex_lock(globals.cleanup_worker_mutex);
	if (task == &globals.cleanup_wake_task) {
		globals.cleanup_wake_queued = SWITCH_FALSE;
	} else if (task && task != &globals.cleanup_stop_task) {
		switch_assert(globals.cleanup_tasks_queued > 0);
		remove_queued_cleanup_task_locked(task);
		globals.cleanup_tasks_queued--;
	}
	stopping = globals.cleanup_worker_stopping;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	return stopping;
}

static void *SWITCH_THREAD_FUNC cleanup_worker(
		switch_thread_t *thread,
		void *data)
{
	(void) thread;
	(void) data;

	for (;;) {
		mws_cleanup_task_t *task = NULL;
		switch_core_session_t *session;
		mws_stream_t *stream_context;
		switch_bool_t discard_task;

		finish_pending_detached_cleanups();
		release_pending_callback_holds();
		if (switch_queue_pop(
					globals.cleanup_tasks,
					(void **) &task) != SWITCH_STATUS_SUCCESS) {
			break;
		}
		discard_task = cleanup_task_popped(task);
		if (task == &globals.cleanup_stop_task) {
			break;
		}
		if (!task || task == &globals.cleanup_wake_task) {
			continue;
		}
		if (discard_task) {
			switch_safe_free(task);
			continue;
		}

		session = switch_core_session_locate(task->identity.session_uuid);
		if (session) {
			stream_context = borrow_registered_stream(
					session,
					&task->identity);
			if (stream_context) {
				wait_for_stream_startup(stream_context);
			}
			if (stream_context && stream_is_registered(stream_context)) {
				(void) stop_registered_stream(
						session,
						stream_context);
			}
			if (stream_context) {
				release_stream_resource_borrow(stream_context);
			}
			switch_core_session_rwunlock(session);
		}
		switch_safe_free(task);
	}

	switch_mutex_lock(globals.cleanup_worker_mutex);
	globals.cleanup_worker_running = SWITCH_FALSE;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	return NULL;
}

static switch_status_t start_cleanup_worker(switch_memory_pool_t *pool)
{
	switch_threadattr_t *thread_attributes;
	switch_status_t status;

	if (switch_mutex_init(
				&globals.cleanup_worker_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_thread_cond_create(
				&globals.cleanup_worker_condition,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_queue_create(
				&globals.cleanup_tasks,
				MWS_CLEANUP_QUEUE_STORAGE_CAPACITY,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_threadattr_create(
				&thread_attributes,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_threadattr_detach_set(thread_attributes, 0);
	switch_threadattr_stacksize_set(
			thread_attributes,
			SWITCH_THREAD_STACKSIZE);
	globals.cleanup_stop_task.identity.session_uuid[0] = '\0';
	globals.cleanup_stop_task.identity.stream_id[0] = '\0';
	globals.cleanup_wake_task.identity.session_uuid[0] = '\0';
	globals.cleanup_wake_task.identity.stream_id[0] = '\0';
	globals.cleanup_worker_running = SWITCH_TRUE;
	globals.cleanup_worker_stopping = SWITCH_FALSE;
	globals.cleanup_stop_queued = SWITCH_FALSE;
	globals.cleanup_wake_queued = SWITCH_FALSE;
	globals.cleanup_tasks_queued = 0;
	globals.queued_cleanup_tasks = NULL;
	globals.pending_detached_cleanups = NULL;
	globals.pending_detached_cleanup_count = 0;
	globals.pending_callback_holds = NULL;
	globals.pending_callback_hold_count = 0;
	globals.cleanup_callback_producers = 0;

	status = switch_thread_create(
			&globals.cleanup_worker_thread,
			thread_attributes,
			cleanup_worker,
			NULL,
			pool);
	if (status != SWITCH_STATUS_SUCCESS) {
		globals.cleanup_worker_thread = NULL;
		globals.cleanup_worker_running = SWITCH_FALSE;
		globals.cleanup_worker_stopping = SWITCH_TRUE;
	}

	return status;
}

static void discard_queued_cleanup_tasks(void)
{
	mws_cleanup_task_t *task;

	switch_mutex_lock(globals.cleanup_worker_mutex);
	task = globals.queued_cleanup_tasks;
	globals.queued_cleanup_tasks = NULL;
	globals.cleanup_tasks_queued = 0;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	while (task) {
		mws_cleanup_task_t *next_task = task->queued_next;

		task->queued_next = NULL;
		switch_safe_free(task);
		task = next_task;
	}
}

static switch_status_t stop_cleanup_worker(void)
{
	switch_thread_t *worker_thread;
	switch_status_t thread_status = SWITCH_STATUS_SUCCESS;
	switch_status_t status;

	switch_mutex_lock(globals.cleanup_worker_mutex);
	worker_thread = globals.cleanup_worker_thread;
	globals.cleanup_worker_stopping = SWITCH_TRUE;
	while (globals.cleanup_callback_producers > 0) {
		switch_thread_cond_wait(
				globals.cleanup_worker_condition,
				globals.cleanup_worker_mutex);
	}
	if (!worker_thread) {
		switch_mutex_unlock(globals.cleanup_worker_mutex);
		finish_pending_detached_cleanups();
		release_pending_callback_holds();
		return SWITCH_STATUS_SUCCESS;
	}
	if (globals.cleanup_stop_queued) {
		status = SWITCH_STATUS_SUCCESS;
	} else {
		status = switch_queue_trypush(
				globals.cleanup_tasks,
				&globals.cleanup_stop_task);
		if (status == SWITCH_STATUS_SUCCESS) {
			globals.cleanup_stop_queued = SWITCH_TRUE;
		}
	}
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_queue_term(globals.cleanup_tasks);
	}

	status = switch_thread_join(&thread_status, worker_thread);
	if (status != SWITCH_STATUS_SUCCESS) {
		return status;
	}
	discard_queued_cleanup_tasks();

	switch_mutex_lock(globals.cleanup_worker_mutex);
	globals.cleanup_worker_thread = NULL;
	globals.cleanup_worker_running = SWITCH_FALSE;
	globals.cleanup_stop_queued = SWITCH_FALSE;
	globals.cleanup_wake_queued = SWITCH_FALSE;
	globals.cleanup_tasks_queued = 0;
	switch_mutex_unlock(globals.cleanup_worker_mutex);
	finish_pending_detached_cleanups();
	release_pending_callback_holds();

	return thread_status;
}

static switch_status_t queue_stream_cleanup(mws_stream_t *stream_context)
{
	mws_cleanup_task_t *task;
	switch_bool_t worker_available;
	switch_status_t status;

	task = calloc(1, sizeof(*task));
	if (!task) {
		return SWITCH_STATUS_MEMERR;
	}
	copy_stream_identity(&task->identity, stream_context);

	switch_mutex_lock(globals.cleanup_worker_mutex);
	worker_available = globals.cleanup_worker_running &&
			!globals.cleanup_worker_stopping &&
			globals.cleanup_tasks_queued < MWS_CLEANUP_QUEUE_CAPACITY;
	if (worker_available) {
		task->queued_next = globals.queued_cleanup_tasks;
		globals.queued_cleanup_tasks = task;
		globals.cleanup_tasks_queued++;
		status = switch_queue_trypush(globals.cleanup_tasks, task);
		if (status != SWITCH_STATUS_SUCCESS) {
			remove_queued_cleanup_task_locked(task);
			globals.cleanup_tasks_queued--;
		}
	} else {
		status = SWITCH_STATUS_FALSE;
	}
	switch_mutex_unlock(globals.cleanup_worker_mutex);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_safe_free(task);
	}

	return status;
}

static void queue_detached_stream_cleanup(
		mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.cleanup_worker_mutex);
	stream_context->pending_cleanup_next =
			globals.pending_detached_cleanups;
	globals.pending_detached_cleanups = stream_context;
	globals.pending_detached_cleanup_count++;
	queue_cleanup_wake_locked();
	switch_mutex_unlock(globals.cleanup_worker_mutex);
}

static void queue_callback_hold(mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.cleanup_worker_mutex);
	stream_context->pending_callback_hold_next =
			globals.pending_callback_holds;
	globals.pending_callback_holds = stream_context;
	globals.pending_callback_hold_count++;
	queue_cleanup_wake_locked();
	switch_mutex_unlock(globals.cleanup_worker_mutex);
}

static void begin_cleanup_callback_producer(void)
{
	switch_mutex_lock(globals.cleanup_worker_mutex);
	globals.cleanup_callback_producers++;
	switch_mutex_unlock(globals.cleanup_worker_mutex);
}

static void end_cleanup_callback_producer(void)
{
	switch_mutex_lock(globals.cleanup_worker_mutex);
	switch_assert(globals.cleanup_callback_producers > 0);
	globals.cleanup_callback_producers--;
	if (!globals.cleanup_callback_producers) {
		switch_thread_cond_broadcast(globals.cleanup_worker_condition);
	}
	switch_mutex_unlock(globals.cleanup_worker_mutex);
}

static switch_status_t worker_accept_peer_close_frame(
		mws_stream_t *stream_context,
		const char *data,
		switch_size_t data_length,
		const struct curl_ws_frame *metadata)
{
	switch_size_t payload_offset;

	if (metadata->offset < 0 || metadata->bytesleft < 0 ||
			(curl_off_t) MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES <
			metadata->offset) {
		return SWITCH_STATUS_FALSE;
	}
	payload_offset = (switch_size_t) metadata->offset;
	if (data_length > MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES -
			payload_offset) {
		return SWITCH_STATUS_FALSE;
	}
	if ((curl_off_t) (MWS_WEBSOCKET_MAXIMUM_CLOSE_PAYLOAD_BYTES -
			payload_offset - data_length) < metadata->bytesleft) {
		return SWITCH_STATUS_FALSE;
	}

	if (!stream_context->normal_close_requested && data_length) {
		memcpy(
				stream_context->close_frame_payload + payload_offset,
				data,
				data_length);
	}

	if (!metadata->bytesleft &&
			!stream_context->normal_close_requested) {
		stream_context->close_frame_payload_length_bytes =
				payload_offset + data_length;
		if (!mws_websocket_close_payload_is_valid(
				stream_context->close_frame_payload,
				stream_context->close_frame_payload_length_bytes)) {
			stream_context->close_frame_payload_length_bytes = 0;
			return SWITCH_STATUS_FALSE;
		}
		stream_context->peer_close_received = SWITCH_TRUE;
		stream_context->normal_close_requested = SWITCH_TRUE;
		stream_context->normal_close_deadline = switch_time_now() +
				MWS_CLOSE_HANDSHAKE_TIMEOUT_US;
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_DEBUG,
				"WebSocket peer close received; reply requested: uuid=%s\n",
				stream_context->session_uuid);
	}

	return SWITCH_STATUS_SUCCESS;
}

static size_t websocket_receive_callback(
		char *data,
		size_t size,
		size_t item_count,
		void *user_data)
{
	mws_stream_t *stream_context = (mws_stream_t *) user_data;
	const struct curl_ws_frame *metadata;
	size_t data_length;

	if (size && item_count > SIZE_MAX / size) {
		stream_context->invalid_inbound_chunk_count++;
		return 0;
	}

	data_length = size * item_count;
	metadata = curl_ws_meta(stream_context->easy);

	if (!metadata) {
		stream_context->invalid_inbound_chunk_count++;
		return data_length;
	}

	worker_mark_transport_open(stream_context);
	if (metadata->flags & CURLWS_BINARY) {
		switch_status_t pcm_status = worker_accept_inbound_pcm(
				stream_context,
				(const uint8_t *) data,
				data_length);

		if (pcm_status == SWITCH_STATUS_BREAK) {
			stream_context->receive_paused = SWITCH_TRUE;
			return CURL_WRITEFUNC_PAUSE;
		}
		if (pcm_status != SWITCH_STATUS_SUCCESS) {
			stream_context->invalid_inbound_chunk_count++;
			return 0;
		}
		stream_context->receive_pause_required_bytes = 0;
	}
	if (metadata->offset == 0) {
		stream_context->inbound_frame_count++;
	}
	stream_context->inbound_byte_count += data_length;

	if (metadata->flags & CURLWS_CLOSE) {
		if (worker_accept_peer_close_frame(
				stream_context,
				data,
				data_length,
				metadata) != SWITCH_STATUS_SUCCESS) {
			stream_context->invalid_inbound_chunk_count++;
			return 0;
		}
	} else if (metadata->flags & CURLWS_BINARY) {
		if (metadata->offset == 0) {
			stream_context->inbound_binary_frame_count++;
		}
		stream_context->inbound_binary_byte_count += data_length;

		if (metadata->bytesleft == 0 &&
				!(metadata->flags & CURLWS_CONT) &&
				!mws_websocket_audio_input_finish_frame(
						&stream_context->websocket_audio_input)) {
			stream_context->invalid_inbound_chunk_count++;
			atomic_fetch_add_explicit(
					&stream_context->playback_invalid_frame_count,
					1,
					memory_order_relaxed);
		}
	} else if (metadata->flags & CURLWS_TEXT) {
		if (metadata->offset == 0) {
			stream_context->inbound_text_frame_count++;
		}
		stream_context->inbound_text_byte_count += data_length;
		worker_accept_inbound_text(
				stream_context,
				data,
				data_length,
				metadata);
	}

	return data_length;
}

static void worker_reset_pcm_state(mws_stream_t *stream_context)
{
	uint32_t capacity_bytes;
	uint64_t discarded_samples;

	mws_captured_audio_resampler_reset(
			&stream_context->captured_audio_resampler);
	mws_playback_audio_resampler_reset(
			&stream_context->playback_audio_resampler);
	mws_websocket_audio_input_reset(
			&stream_context->websocket_audio_input);
	discarded_samples = mws_source_audio_buffer_discard_all(
			&stream_context->source_audio_buffer) / sizeof(int16_t);
	atomic_fetch_add_explicit(
			&stream_context->playback_source_dropped_sample_count,
			discarded_samples,
			memory_order_relaxed);

	capacity_bytes = (uint32_t) calculate_pcm_capacity_bytes(
			atomic_load_explicit(
				&stream_context->playback_sample_rate,
				memory_order_acquire),
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	discarded_samples = worker_reset_playback_buffer(
			stream_context,
			capacity_bytes);
	atomic_fetch_add_explicit(
			&stream_context->playback_dropped_sample_count,
			discarded_samples,
			memory_order_relaxed);
	atomic_store_explicit(
			&stream_context->playback_state,
			MWS_PLAYBACK_BUFFERING,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_overflow_active,
			0,
			memory_order_release);
	atomic_store_explicit(
			&stream_context->playback_format_reset_requested,
			0,
			memory_order_release);

	mws_captured_audio_buffer_discard_fallback(
			&stream_context->captured_audio_buffer);
}

static void worker_discard_captured_audio(mws_stream_t *stream_context)
{
	(void) mws_captured_audio_buffer_discard_ring(
			&stream_context->captured_audio_buffer);
}

static void worker_reset_connection_attempt(mws_stream_t *stream_context)
{
	stream_context->start_message_offset_bytes = 0;
	stream_context->start_message_frame_started = SWITCH_FALSE;
	stream_context->start_message_sent = SWITCH_FALSE;
	stream_context->upload_paused = SWITCH_FALSE;
	stream_context->receive_paused = SWITCH_FALSE;
	stream_context->receive_pause_required_bytes = 0;
	stream_context->normal_close_requested = SWITCH_FALSE;
	stream_context->close_frame_started = SWITCH_FALSE;
	stream_context->close_frame_sent = SWITCH_FALSE;
	stream_context->close_frame_payload_length_bytes = 0;
	stream_context->close_frame_offset_bytes = 0;
	stream_context->normal_close_deadline = 0;
	stream_context->next_statistics_event_at = 0;
	stream_context->inbound_text_message_length_bytes = 0;
	stream_context->inbound_text_message_active = SWITCH_FALSE;
	stream_context->inbound_text_message_too_large = SWITCH_FALSE;
	stream_context->dropped_text_response_count +=
			stream_context->text_response_count;
	stream_context->text_response_read_index = 0;
	stream_context->text_response_write_index = 0;
	stream_context->text_response_count = 0;
	stream_context->text_response_offset_bytes = 0;
	stream_context->text_response_frame_started = SWITCH_FALSE;
	stream_context->peer_close_received = SWITCH_FALSE;
	stream_context->outbound_message_frame_started = SWITCH_FALSE;
	stream_context->outbound_message_offset_bytes = 0;
	switch_mutex_lock(stream_context->outbound_message_mutex);
	stream_context->dropped_outbound_message_count +=
			stream_context->outbound_message_count;
	stream_context->outbound_message_read_index = 0;
	stream_context->outbound_message_write_index = 0;
	stream_context->outbound_message_count = 0;
	switch_mutex_unlock(stream_context->outbound_message_mutex);
}

static uint32_t calculate_reconnect_delay_ms(mws_stream_t *stream_context)
{
	uint64_t delay_ms = (uint64_t) stream_context->reconnect_delay_ms;
	uint32_t doubling_count =
			stream_context->consecutive_reconnect_attempts > 0 ?
			stream_context->consecutive_reconnect_attempts - 1 : 0;

	while (doubling_count-- && delay_ms < MWS_MAXIMUM_RECONNECT_DELAY_MS) {
		delay_ms *= 2;
		if (delay_ms > MWS_MAXIMUM_RECONNECT_DELAY_MS) {
			delay_ms = MWS_MAXIMUM_RECONNECT_DELAY_MS;
		}
	}

	return (uint32_t) delay_ms;
}

static size_t worker_upload_start_message(
		mws_stream_t *stream_context,
		char *data,
		switch_size_t upload_capacity_bytes)
{
	switch_size_t remaining_bytes;
	switch_size_t upload_length_bytes;
	CURLcode result;

	if (!stream_context->start_message ||
			!stream_context->start_message_length_bytes ||
			!upload_capacity_bytes) {
		return CURL_READFUNC_ABORT;
	}

	if (!stream_context->start_message_frame_started) {
		result = curl_ws_start_frame(
				stream_context->easy,
				CURLWS_TEXT,
				(curl_off_t) stream_context->start_message_length_bytes);
		if (result == CURLE_AGAIN) {
			stream_context->upload_paused = SWITCH_TRUE;
			return CURL_READFUNC_PAUSE;
		}
		if (result != CURLE_OK) {
			stream_context->curl_result = result;
			return CURL_READFUNC_ABORT;
		}
		stream_context->start_message_frame_started = SWITCH_TRUE;
	}

	remaining_bytes = stream_context->start_message_length_bytes -
			stream_context->start_message_offset_bytes;
	upload_length_bytes = remaining_bytes < upload_capacity_bytes ?
			remaining_bytes : upload_capacity_bytes;

	memcpy(
			data,
			stream_context->start_message +
					stream_context->start_message_offset_bytes,
			upload_length_bytes);
	stream_context->start_message_offset_bytes += upload_length_bytes;

	if (stream_context->start_message_offset_bytes ==
			stream_context->start_message_length_bytes) {
		stream_context->start_message_frame_started = SWITCH_FALSE;
		stream_context->start_message_sent = SWITCH_TRUE;
		stream_context->text_frame_sent_count++;
		stream_context->text_byte_sent_count +=
				stream_context->start_message_length_bytes;
	}

	stream_context->upload_paused = SWITCH_FALSE;
	return upload_length_bytes;
}

static size_t worker_upload_api_command_result(
		mws_stream_t *stream_context,
		char *data,
		switch_size_t upload_capacity_bytes)
{
	switch_size_t remaining_bytes;
	switch_size_t upload_length_bytes;
	switch_size_t response_length_bytes;
	CURLcode result;

	if (!stream_context->api_command_result_ready ||
			!stream_context->api_command_result ||
			!stream_context->api_command_result_length_bytes ||
			!upload_capacity_bytes) {
		return CURL_READFUNC_ABORT;
	}

	response_length_bytes =
			stream_context->api_command_result_length_bytes;
	if (!stream_context->api_command_result_frame_started) {
		result = curl_ws_start_frame(
				stream_context->easy,
				CURLWS_TEXT,
				(curl_off_t) response_length_bytes);
		if (result == CURLE_AGAIN) {
			stream_context->upload_paused = SWITCH_TRUE;
			return CURL_READFUNC_PAUSE;
		}
		if (result != CURLE_OK) {
			stream_context->curl_result = result;
			return CURL_READFUNC_ABORT;
		}
		stream_context->api_command_result_frame_started = SWITCH_TRUE;
	}

	remaining_bytes = response_length_bytes -
			stream_context->api_command_result_offset_bytes;
	upload_length_bytes = remaining_bytes < upload_capacity_bytes ?
			remaining_bytes : upload_capacity_bytes;
	memcpy(
			data,
			stream_context->api_command_result +
					stream_context->api_command_result_offset_bytes,
			upload_length_bytes);
	stream_context->api_command_result_offset_bytes += upload_length_bytes;

	if (stream_context->api_command_result_offset_bytes ==
			response_length_bytes) {
		stream_context->text_frame_sent_count++;
		stream_context->text_byte_sent_count += response_length_bytes;

		switch_mutex_lock(stream_context->api_command_mutex);
		switch_safe_free(stream_context->api_command_result);
		stream_context->api_command_result_length_bytes = 0;
		stream_context->api_command_result_offset_bytes = 0;
		stream_context->api_command_result_frame_started = SWITCH_FALSE;
		stream_context->api_command_result_ready = SWITCH_FALSE;
		stream_context->api_command_active = SWITCH_FALSE;
		switch_mutex_unlock(stream_context->api_command_mutex);
	}

	stream_context->upload_paused = SWITCH_FALSE;
	return upload_length_bytes;
}

static size_t worker_upload_text_response(
		mws_stream_t *stream_context,
		char *data,
		switch_size_t upload_capacity_bytes)
{
	const char *response;
	switch_size_t response_length_bytes;
	switch_size_t remaining_bytes;
	switch_size_t upload_length_bytes;
	CURLcode result;

	if (!stream_context->text_response_count || !upload_capacity_bytes) {
		return CURL_READFUNC_ABORT;
	}

	response = stream_context->text_response_queue
			[stream_context->text_response_read_index];
	response_length_bytes = stream_context->text_response_lengths
			[stream_context->text_response_read_index];

	if (!stream_context->text_response_frame_started) {
		result = curl_ws_start_frame(
				stream_context->easy,
				CURLWS_TEXT,
				(curl_off_t) response_length_bytes);
		if (result == CURLE_AGAIN) {
			stream_context->upload_paused = SWITCH_TRUE;
			return CURL_READFUNC_PAUSE;
		}
		if (result != CURLE_OK) {
			stream_context->curl_result = result;
			return CURL_READFUNC_ABORT;
		}
		stream_context->text_response_frame_started = SWITCH_TRUE;
	}

	remaining_bytes = response_length_bytes -
			stream_context->text_response_offset_bytes;
	upload_length_bytes = remaining_bytes < upload_capacity_bytes ?
			remaining_bytes : upload_capacity_bytes;

	memcpy(
			data,
			response + stream_context->text_response_offset_bytes,
			upload_length_bytes);
	stream_context->text_response_offset_bytes += upload_length_bytes;

	if (stream_context->text_response_offset_bytes ==
			response_length_bytes) {
		stream_context->text_response_frame_started = SWITCH_FALSE;
		stream_context->text_response_offset_bytes = 0;
		stream_context->text_response_read_index =
				(stream_context->text_response_read_index + 1) %
				MWS_TEXT_RESPONSE_QUEUE_CAPACITY;
		stream_context->text_response_count--;
		stream_context->text_frame_sent_count++;
		stream_context->text_byte_sent_count += response_length_bytes;
	}

	stream_context->upload_paused = SWITCH_FALSE;
	return upload_length_bytes;
}

static switch_bool_t worker_has_outbound_message(mws_stream_t *stream_context)
{
	switch_bool_t has_message;

	switch_mutex_lock(stream_context->outbound_message_mutex);
	has_message = stream_context->outbound_message_count ?
			SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(stream_context->outbound_message_mutex);

	return has_message;
}

/*
 * The producer only writes the slot at write_index, so the slot at
 * read_index can be read without the mutex while the frame is uploaded.
 */
static size_t worker_upload_outbound_message(
		mws_stream_t *stream_context,
		char *data,
		switch_size_t upload_capacity_bytes)
{
	const char *message;
	switch_size_t message_length_bytes;
	switch_size_t remaining_bytes;
	switch_size_t upload_length_bytes;
	CURLcode result;

	if (!upload_capacity_bytes ||
			!worker_has_outbound_message(stream_context)) {
		return CURL_READFUNC_ABORT;
	}

	message = stream_context->outbound_messages
			[stream_context->outbound_message_read_index];
	message_length_bytes = stream_context->outbound_message_lengths
			[stream_context->outbound_message_read_index];

	if (!stream_context->outbound_message_frame_started) {
		result = curl_ws_start_frame(
				stream_context->easy,
				CURLWS_TEXT,
				(curl_off_t) message_length_bytes);
		if (result == CURLE_AGAIN) {
			stream_context->upload_paused = SWITCH_TRUE;
			return CURL_READFUNC_PAUSE;
		}
		if (result != CURLE_OK) {
			stream_context->curl_result = result;
			return CURL_READFUNC_ABORT;
		}
		stream_context->outbound_message_frame_started = SWITCH_TRUE;
	}

	remaining_bytes = message_length_bytes -
			stream_context->outbound_message_offset_bytes;
	upload_length_bytes = remaining_bytes < upload_capacity_bytes ?
			remaining_bytes : upload_capacity_bytes;

	memcpy(
			data,
			message + stream_context->outbound_message_offset_bytes,
			upload_length_bytes);
	stream_context->outbound_message_offset_bytes += upload_length_bytes;

	if (stream_context->outbound_message_offset_bytes ==
			message_length_bytes) {
		stream_context->outbound_message_frame_started = SWITCH_FALSE;
		stream_context->outbound_message_offset_bytes = 0;
		switch_mutex_lock(stream_context->outbound_message_mutex);
		stream_context->outbound_message_read_index =
				(stream_context->outbound_message_read_index + 1) %
				MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY;
		stream_context->outbound_message_count--;
		switch_mutex_unlock(stream_context->outbound_message_mutex);
		atomic_fetch_add_explicit(
				&stream_context->outbound_message_sent_count,
				1,
				memory_order_relaxed);
		stream_context->text_frame_sent_count++;
		stream_context->text_byte_sent_count += message_length_bytes;
	}

	stream_context->upload_paused = SWITCH_FALSE;
	return upload_length_bytes;
}

static size_t worker_upload_normal_close_frame(
		mws_stream_t *stream_context,
		char *data,
		switch_size_t upload_capacity_bytes)
{
	switch_size_t remaining_bytes;
	switch_size_t upload_length_bytes;
	CURLcode result;

	if (stream_context->close_frame_sent) {
		stream_context->upload_paused = SWITCH_TRUE;
		return CURL_READFUNC_PAUSE;
	}
	if (!upload_capacity_bytes) {
		return CURL_READFUNC_ABORT;
	}

	if (!stream_context->close_frame_started) {
		result = curl_ws_start_frame(
				stream_context->easy,
				CURLWS_CLOSE,
				(curl_off_t)
						stream_context->close_frame_payload_length_bytes);
		if (result == CURLE_AGAIN) {
			stream_context->upload_paused = SWITCH_TRUE;
			return CURL_READFUNC_PAUSE;
		}
		if (result != CURLE_OK) {
			stream_context->curl_result = result;
			return CURL_READFUNC_ABORT;
		}
		stream_context->close_frame_started = SWITCH_TRUE;
	}

	remaining_bytes = stream_context->close_frame_payload_length_bytes -
			stream_context->close_frame_offset_bytes;
	upload_length_bytes = remaining_bytes < upload_capacity_bytes ?
			remaining_bytes : upload_capacity_bytes;
	memcpy(
			data,
			stream_context->close_frame_payload +
					stream_context->close_frame_offset_bytes,
			upload_length_bytes);
	stream_context->close_frame_offset_bytes += upload_length_bytes;

	if (stream_context->close_frame_offset_bytes ==
			stream_context->close_frame_payload_length_bytes) {
		stream_context->close_frame_started = SWITCH_FALSE;
		stream_context->close_frame_sent = SWITCH_TRUE;
	}

	stream_context->upload_paused = SWITCH_FALSE;
	return upload_length_bytes;
}

static size_t websocket_upload_callback(
		char *data,
		size_t size,
		size_t item_count,
		void *user_data)
{
	mws_stream_t *stream_context = (mws_stream_t *) user_data;
	mws_captured_audio_resampler_result_t resampler_result;
	switch_size_t upload_capacity_bytes;
	switch_size_t upload_length_bytes;
	switch_bool_t api_command_active;
	switch_bool_t api_command_result_ready;

	if (size && item_count > SIZE_MAX / size) {
		return CURL_READFUNC_ABORT;
	}

	worker_mark_transport_open(stream_context);
	upload_capacity_bytes = size * item_count;
	if (stream_context->normal_close_requested) {
		if (stream_context->start_message_frame_started) {
			return worker_upload_start_message(
					stream_context,
					data,
					upload_capacity_bytes);
		}
		if (stream_context->api_command_result_frame_started) {
			return worker_upload_api_command_result(
					stream_context,
					data,
					upload_capacity_bytes);
		}
		if (stream_context->text_response_frame_started) {
			return worker_upload_text_response(
					stream_context,
					data,
					upload_capacity_bytes);
		}
		/* Deliver queued application messages before closing. */
		if (stream_context->start_message_sent &&
				!stream_context->peer_close_received &&
				!stream_context->close_frame_started &&
				!stream_context->close_frame_sent &&
				(stream_context->outbound_message_frame_started ||
				 worker_has_outbound_message(stream_context))) {
			return worker_upload_outbound_message(
					stream_context,
					data,
					upload_capacity_bytes);
		}
		return worker_upload_normal_close_frame(
				stream_context,
				data,
				upload_capacity_bytes);
	}
	if (!stream_context->start_message_sent) {
		return worker_upload_start_message(
				stream_context,
				data,
				upload_capacity_bytes);
	}
	switch_mutex_lock(stream_context->api_command_mutex);
	api_command_active = stream_context->api_command_active;
	api_command_result_ready = stream_context->api_command_result_ready;
	switch_mutex_unlock(stream_context->api_command_mutex);
	if (api_command_result_ready) {
		return worker_upload_api_command_result(
				stream_context,
				data,
				upload_capacity_bytes);
	}
	if (stream_context->text_response_count &&
			!api_command_active) {
		return worker_upload_text_response(
				stream_context,
				data,
				upload_capacity_bytes);
	}
	if (stream_context->outbound_message_frame_started ||
			worker_has_outbound_message(stream_context)) {
		return worker_upload_outbound_message(
				stream_context,
				data,
				upload_capacity_bytes);
	}

	resampler_result = mws_captured_audio_resampler_read(
			&stream_context->captured_audio_resampler,
			&stream_context->captured_audio_buffer,
			(uint8_t *) data,
			upload_capacity_bytes,
			&upload_length_bytes);
	if (resampler_result == MWS_CAPTURED_AUDIO_RESAMPLER_ERROR) {
		return CURL_READFUNC_ABORT;
	}
	if (resampler_result == MWS_CAPTURED_AUDIO_RESAMPLER_EMPTY) {
		stream_context->upload_paused = SWITCH_TRUE;
		return CURL_READFUNC_PAUSE;
	}

	convert_native_pcm_to_little_endian(
			(uint8_t *) data,
			upload_length_bytes);
	stream_context->upload_paused = SWITCH_FALSE;
	stream_context->libcurl_submitted_frame_count++;
	stream_context->libcurl_submitted_byte_count += upload_length_bytes;

	return upload_length_bytes;
}

static CURLMcode worker_cleanup_easy(
		CURLM *multi,
		mws_stream_t *stream_context)
{
	CURLMcode result;

	api_command_abandon_result(stream_context);

	if (!stream_context->easy) {
		curl_slist_free_all(stream_context->curl_request_headers);
		stream_context->curl_request_headers = NULL;
		return CURLM_OK;
	}

	if (stream_context->easy_added) {
		result = curl_multi_remove_handle(multi, stream_context->easy);
		if (result != CURLM_OK) {
			stream_context->multi_result = result;
			stream_context->transport_terminal_state = MWS_TRANSPORT_FAILED;
			switch_log_printf(
					SWITCH_CHANNEL_LOG,
					SWITCH_LOG_CRIT,
					"Could not remove WebSocket easy handle: "
					"uuid=%s multi=%s\n",
					stream_context->session_uuid,
					curl_multi_strerror(result));
			return result;
		}
		stream_context->easy_added = SWITCH_FALSE;
	}

	curl_easy_cleanup(stream_context->easy);
	stream_context->easy = NULL;
	curl_slist_free_all(stream_context->curl_request_headers);
	stream_context->curl_request_headers = NULL;
	stream_context->upload_paused = SWITCH_FALSE;
	stream_context->receive_paused = SWITCH_FALSE;
	stream_context->receive_pause_required_bytes = 0;

	return CURLM_OK;
}

static switch_status_t worker_build_curl_request_headers(
		mws_stream_t *stream_context)
{
	mws_stream_request_header_t *header;

	for (header = stream_context->request_headers; header;
			header = header->next) {
		struct curl_slist *appended_headers;
		char *header_line = switch_mprintf(
				"%s: %s",
				header->name,
				header->value);

		if (!header_line) {
			goto failed;
		}
		appended_headers = curl_slist_append(
				stream_context->curl_request_headers,
				header_line);
		switch_safe_free(header_line);
		if (!appended_headers) {
			goto failed;
		}
		stream_context->curl_request_headers = appended_headers;
	}

	return SWITCH_STATUS_SUCCESS;

failed:
	curl_slist_free_all(stream_context->curl_request_headers);
	stream_context->curl_request_headers = NULL;
	return SWITCH_STATUS_MEMERR;
}

static void set_optional_channel_uint64_variable(
		switch_channel_t *channel,
		const char *name,
		bool available,
		uint64_t value)
{
	if (available) {
		switch_channel_set_variable_printf(
				channel,
				name,
				"%" SWITCH_UINT64_T_FMT,
				value);
	} else {
		switch_channel_set_variable(channel, name, NULL);
	}
}

static void set_final_stream_statistics_variables(
		switch_channel_t *channel,
		mws_stream_t *stream_context)
{
	mws_stream_statistics_t statistics;

	snapshot_stream_statistics(stream_context, &statistics);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_TARGET_RATE_HZ_VARIABLE,
			"%d",
			stream_context->target_sample_rate);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_CAPTURED_DROP_EVENTS_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_drop_events);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_CAPTURED_DROPPED_BYTES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.captured_dropped_bytes);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RECEIVED_SAMPLES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_received_samples);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_SOURCE_DROPPED_SAMPLES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_source_dropped_samples);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_SOURCE_CAPACITY_BYTES_VARIABLE,
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_source_capacity_bytes);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_BUFFERED_SAMPLES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_buffered_samples);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_MIXED_SAMPLES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_mixed_samples);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_DROPPED_SAMPLES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_dropped_samples);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_PEAK_QUEUED_BYTES_VARIABLE,
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_peak_queued_bytes);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_CAPACITY_BYTES_VARIABLE,
			"%" SWITCH_SIZE_T_FMT,
			statistics.playback_capacity_bytes);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_FRAMES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_underflow_frames);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_INVALID_FRAMES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_invalid_frames);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_ADJUSTMENT_PPM_VARIABLE,
			"%d",
			statistics.playback_rate_adjustment_ppm);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_MAXIMUM_ADJUSTMENT_PPM_VARIABLE,
			"%u",
			statistics.playback_rate_maximum_adjustment_ppm);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_FILTERED_QUEUE_US_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_rate_filtered_queue_us);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_TARGET_QUEUE_US_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_rate_target_queue_us);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_UPDATES_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_rate_updates);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_RATE_LIMIT_DURATION_US_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_rate_limit_duration_us);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_CALLBACK_MAX_LATENESS_US_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_max_lateness_us);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_CALLBACK_GAP_INTERVALS_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_callback_gap_intervals);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_MODULE_DISPATCH_MAX_DELAY_US_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_module_dispatch_max_delay_us);
	switch_channel_set_variable_printf(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_WITH_SOURCE_AUDIO_COUNT_VARIABLE,
			"%" SWITCH_UINT64_T_FMT,
			statistics.playback_underflow_with_source_audio_count);

	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_MAX_GAP_AGE_US_VARIABLE,
			statistics.playback_maximum_gap.valid,
			statistics.playback_maximum_gap.observation_age_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_MAX_GAP_LATENESS_US_VARIABLE,
			statistics.playback_maximum_gap.valid,
			statistics.playback_maximum_gap.callback_lateness_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_MAX_GAP_DISPATCH_DELAY_US_VARIABLE,
			statistics.playback_maximum_gap.valid,
			statistics.playback_maximum_gap.module_dispatch_delay_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_MAX_GAP_SOURCE_QUEUED_BYTES_VARIABLE,
			statistics.playback_maximum_gap.valid,
			statistics.playback_maximum_gap.source_queued_bytes);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_MAX_GAP_QUEUED_BYTES_VARIABLE,
			statistics.playback_maximum_gap.valid,
			statistics.playback_maximum_gap.playback_queued_bytes);

	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_AGE_US_VARIABLE,
			statistics.playback_last_underflow.valid,
			statistics.playback_last_underflow.observation_age_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_CALLBACK_LATENESS_US_VARIABLE,
			statistics.playback_last_underflow.valid,
			statistics.playback_last_underflow.callback_lateness_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_DISPATCH_DELAY_US_VARIABLE,
			statistics.playback_last_underflow.valid,
			statistics.playback_last_underflow.module_dispatch_delay_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_SOURCE_QUEUED_BYTES_VARIABLE,
			statistics.playback_last_underflow.valid,
			statistics.playback_last_underflow.source_queued_bytes);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_QUEUED_BYTES_VARIABLE,
			statistics.playback_last_underflow.valid,
			statistics.playback_last_underflow.playback_queued_bytes);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_WEBSOCKET_AUDIO_AGE_US_VARIABLE,
			statistics.playback_last_underflow.valid &&
				statistics.playback_last_underflow.websocket_audio_seen,
			statistics.playback_last_underflow.websocket_audio_age_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_BUFFER_WRITE_AGE_US_VARIABLE,
			statistics.playback_last_underflow.valid &&
				statistics.playback_last_underflow.playback_buffer_write_seen,
			statistics.playback_last_underflow.playback_buffer_write_age_us);
	set_optional_channel_uint64_variable(
			channel,
			MWS_LAST_PLAYBACK_UNDERFLOW_WORKER_VISIT_AGE_US_VARIABLE,
			statistics.playback_last_underflow.valid &&
				statistics.playback_last_underflow.worker_visit_seen,
			statistics.playback_last_underflow.worker_visit_age_us);
}

static void worker_report_terminal_result(
		mws_stream_t *stream_context,
		const char *error_message)
{
	const char *terminal_state = transport_state_name(
			stream_context->transport_terminal_state);
	const char *safe_error_message = zstr(error_message) ?
			"unknown" : error_message;
	switch_core_session_t *session;
	char curl_code[32];

	if (stream_context->terminal_result_reported) {
		return;
	}
	stream_context->terminal_result_reported = SWITCH_TRUE;

	switch_snprintf(
			curl_code,
			sizeof(curl_code),
			"%d",
			(int) stream_context->curl_result);

	if ((session = switch_core_session_locate(stream_context->session_uuid))) {
		switch_channel_t *channel = switch_core_session_get_channel(session);

		if (stream_is_registered(stream_context)) {
			switch_channel_set_variable(
					channel,
					MWS_TRANSPORT_STATE_VARIABLE,
					terminal_state);
			switch_channel_set_variable(
					channel,
					MWS_CURL_CODE_VARIABLE,
					curl_code);
			switch_channel_set_variable(
					channel,
					MWS_CURL_ERROR_VARIABLE,
					safe_error_message);
			switch_channel_set_variable(
					channel,
					MWS_LAST_STREAM_ID_VARIABLE,
					stream_context->stream_id);
			set_final_stream_statistics_variables(channel, stream_context);
		}
		switch_core_session_rwunlock(session);
	}

	worker_fire_transport_event(
			stream_context,
			stream_context->transport_terminal_state,
			stream_context->curl_result,
			stream_context->multi_result,
			safe_error_message);
}

static void worker_request_automatic_stream_cleanup(
		mws_stream_t *stream_context,
		const char *error_message)
{
	if (switch_atomic_read(&stream_context->automatic_cleanup_requested)) {
		return;
	}

	worker_report_terminal_result(stream_context, error_message);

	switch_atomic_set(&stream_context->automatic_cleanup_requested, 1);
	switch_mutex_lock(globals.worker_mutex);
	globals.automatic_cleanup_request_count++;
	switch_mutex_unlock(globals.worker_mutex);

	if (queue_stream_cleanup(stream_context) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"Could not queue automatic WebSocket stream cleanup: "
				"uuid=%s stream_id=%s\n",
				stream_context->session_uuid,
				stream_context->stream_id);
	}
}

static void worker_fail_transport_setup(
		mws_stream_t *stream_context,
		CURLcode curl_result,
		CURLMcode multi_result)
{
	const char *error_message = curl_result != CURLE_OK ?
			curl_easy_strerror(curl_result) :
			curl_multi_strerror(multi_result);

	worker_reset_pcm_state(stream_context);
	stream_context->curl_result = curl_result;
	stream_context->multi_result = multi_result;
	stream_context->transport_terminal_state = MWS_TRANSPORT_FAILED;
	worker_set_transport_state(stream_context, MWS_TRANSPORT_FAILED);
	worker_request_automatic_stream_cleanup(
			stream_context,
			error_message);

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_ERROR,
			"Could not start WebSocket connection: uuid=%s "
			"curl=%s multi=%s\n",
			stream_context->session_uuid,
			curl_easy_strerror(curl_result),
			curl_multi_strerror(multi_result));
}

static void worker_start_transport(
		CURLM *multi,
		mws_stream_t *stream_context)
{
	CURLcode curl_result;
	CURLMcode multi_result;

	stream_context->easy = curl_easy_init();
	if (!stream_context->easy) {
		worker_fail_transport_setup(
				stream_context,
				CURLE_FAILED_INIT,
				CURLM_OK);
		return;
	}

	stream_context->curl_error[0] = '\0';
	stream_context->curl_result = CURLE_OK;
	stream_context->multi_result = CURLM_OK;
	if (worker_build_curl_request_headers(stream_context) !=
			SWITCH_STATUS_SUCCESS) {
		(void) worker_cleanup_easy(multi, stream_context);
		worker_fail_transport_setup(
				stream_context,
				CURLE_OUT_OF_MEMORY,
				CURLM_OK);
		return;
	}

#define MWS_CURL_SETOPT(option, value) \
	do { \
		curl_result = curl_easy_setopt( \
				stream_context->easy, (option), (value)); \
		if (curl_result != CURLE_OK) { \
			goto setopt_failed; \
		} \
	} while (0)

	MWS_CURL_SETOPT(CURLOPT_ERRORBUFFER, stream_context->curl_error);
	MWS_CURL_SETOPT(CURLOPT_URL, stream_context->url);
	MWS_CURL_SETOPT(CURLOPT_PRIVATE, stream_context);
	MWS_CURL_SETOPT(CURLOPT_NOSIGNAL, 1L);
	MWS_CURL_SETOPT(CURLOPT_TCP_NODELAY, 1L);
	MWS_CURL_SETOPT(CURLOPT_CONNECTTIMEOUT_MS, MWS_CONNECT_TIMEOUT_MS);
	MWS_CURL_SETOPT(CURLOPT_USERAGENT, MWS_USER_AGENT);
	if (stream_context->curl_request_headers) {
		MWS_CURL_SETOPT(
				CURLOPT_HTTPHEADER,
				stream_context->curl_request_headers);
	}
	MWS_CURL_SETOPT(CURLOPT_WRITEFUNCTION, websocket_receive_callback);
	MWS_CURL_SETOPT(CURLOPT_WRITEDATA, stream_context);
	MWS_CURL_SETOPT(CURLOPT_READFUNCTION, websocket_upload_callback);
	MWS_CURL_SETOPT(CURLOPT_READDATA, stream_context);
	MWS_CURL_SETOPT(CURLOPT_UPLOAD, 1L);

#undef MWS_CURL_SETOPT

	multi_result = curl_multi_add_handle(multi, stream_context->easy);
	if (multi_result != CURLM_OK) {
		(void) worker_cleanup_easy(multi, stream_context);
		worker_fail_transport_setup(
				stream_context,
				CURLE_OK,
				multi_result);
		return;
	}

	stream_context->easy_added = SWITCH_TRUE;
	worker_set_transport_state(stream_context, MWS_TRANSPORT_CONNECTING);

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_NOTICE,
			"WebSocket connection starting: uuid=%s\n",
			stream_context->session_uuid);
	return;

setopt_failed:
#undef MWS_CURL_SETOPT
	(void) worker_cleanup_easy(multi, stream_context);
	worker_fail_transport_setup(
			stream_context,
			curl_result,
			CURLM_OK);
}

static switch_bool_t worker_schedule_reconnect(
		mws_stream_t *stream_context,
		const char *error_message)
{
	uint32_t delay_ms;

	if (atomic_load_explicit(
			&stream_context->shutdown_requested,
			memory_order_acquire) ||
			stream_context->worker_removal_requested ||
			stream_context->normal_close_requested ||
			stream_context->reconnect_attempt_limit <= 0 ||
			stream_context->consecutive_reconnect_attempts >=
					(uint32_t) stream_context->reconnect_attempt_limit) {
		return SWITCH_FALSE;
	}

	switch_mutex_lock(globals.worker_mutex);
	stream_context->consecutive_reconnect_attempts++;
	stream_context->reconnect_attempt_count++;
	globals.reconnect_attempt_count++;
	delay_ms = calculate_reconnect_delay_ms(stream_context);
	stream_context->next_reconnect_at = switch_time_now() +
			(switch_time_t) delay_ms * 1000;
	switch_mutex_unlock(globals.worker_mutex);

	worker_set_transport_state(stream_context, MWS_TRANSPORT_RECONNECTING);
	worker_fire_transport_event(
			stream_context,
			MWS_TRANSPORT_RECONNECTING,
			stream_context->curl_result,
			stream_context->multi_result,
			error_message);

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_WARNING,
			"WebSocket reconnect scheduled: uuid=%s stream_id=%s "
			"attempt=%u/%d delay_ms=%u error=%s\n",
			stream_context->session_uuid,
			stream_context->stream_id,
			stream_context->consecutive_reconnect_attempts,
			stream_context->reconnect_attempt_limit,
			delay_ms,
			zstr(error_message) ? "unknown" : error_message);

	return SWITCH_TRUE;
}

static switch_bool_t worker_finish_transport(
		CURLM *multi,
		mws_stream_t *stream_context,
		CURLcode result)
{
	const char *error_message;
	CURLMcode multi_result;

	stream_context->curl_result = result;
	error_message = stream_context->curl_error[0] ?
			stream_context->curl_error : curl_easy_strerror(result);

	multi_result = worker_cleanup_easy(multi, stream_context);
	worker_reset_pcm_state(stream_context);
	if (multi_result != CURLM_OK) {
		worker_set_transport_state(stream_context, MWS_TRANSPORT_FAILED);
		return SWITCH_FALSE;
	}

	if (result == CURLE_OK) {
		stream_context->transport_terminal_state = MWS_TRANSPORT_CLOSED;
		worker_set_transport_state(stream_context, MWS_TRANSPORT_CLOSED);
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_NOTICE,
				"WebSocket connection closed: uuid=%s\n",
				stream_context->session_uuid);
	} else if (worker_schedule_reconnect(stream_context, error_message)) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"WebSocket connection interrupted: uuid=%s "
				"curl=%d error=%s\n",
				stream_context->session_uuid,
				(int) result,
				error_message);
		return SWITCH_TRUE;
	} else {
		stream_context->transport_terminal_state = MWS_TRANSPORT_FAILED;
		worker_set_transport_state(stream_context, MWS_TRANSPORT_FAILED);
		if (!stream_context->normal_close_requested &&
				stream_context->reconnect_attempt_limit > 0 &&
				stream_context->consecutive_reconnect_attempts >=
						(uint32_t) stream_context->reconnect_attempt_limit) {
			switch_mutex_lock(globals.worker_mutex);
			globals.reconnect_exhausted_count++;
			switch_mutex_unlock(globals.worker_mutex);
		}
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"WebSocket connection failed: uuid=%s "
				"curl=%d error=%s\n",
				stream_context->session_uuid,
				(int) result,
				error_message);
	}
	if (!stream_context->worker_removal_requested) {
		worker_request_automatic_stream_cleanup(
				stream_context,
				error_message);
	}

	return SWITCH_TRUE;
}

static void worker_start_due_reconnects(
		CURLM *multi,
		mws_stream_t *worker_streams)
{
	mws_stream_t *stream_context;
	switch_time_t now = switch_time_now();

	for (stream_context = worker_streams; stream_context;
			stream_context = stream_context->worker_next) {
		if (stream_context->transport_state != MWS_TRANSPORT_RECONNECTING ||
				!stream_context->next_reconnect_at ||
				now < stream_context->next_reconnect_at) {
			continue;
		}
		if (atomic_load_explicit(
				&stream_context->shutdown_requested,
				memory_order_acquire)) {
			continue;
		}

		worker_discard_captured_audio(stream_context);
		worker_reset_connection_attempt(stream_context);
		worker_start_transport(multi, stream_context);
	}
}

static switch_bool_t worker_has_upload_data(
		mws_stream_t *stream_context)
{
	switch_bool_t api_command_active;
	switch_bool_t api_command_result_ready;

	if (stream_context->normal_close_requested) {
		return stream_context->close_frame_sent ? SWITCH_FALSE : SWITCH_TRUE;
	}
	if (!stream_context->start_message_sent) {
		return SWITCH_TRUE;
	}
	switch_mutex_lock(stream_context->api_command_mutex);
	api_command_active = stream_context->api_command_active;
	api_command_result_ready = stream_context->api_command_result_ready;
	switch_mutex_unlock(stream_context->api_command_mutex);
	if (api_command_result_ready ||
			(stream_context->text_response_count &&
			 !api_command_active) ||
			worker_has_outbound_message(stream_context)) {
		return SWITCH_TRUE;
	}

	if (mws_captured_audio_resampler_has_pending_output(
			&stream_context->captured_audio_resampler)) {
		return SWITCH_TRUE;
	}
	return mws_captured_audio_buffer_has_data(
			&stream_context->captured_audio_buffer);
}

static switch_bool_t worker_resume_stream(
		CURLM *multi,
		mws_stream_t *stream_context)
{
	CURLcode result;
	switch_bool_t resume_receive;
	switch_bool_t resume_upload;
	int pause_action = CURLPAUSE_CONT;

	if (stream_context->transport_state != MWS_TRANSPORT_OPEN ||
			!stream_context->easy) {
		return SWITCH_TRUE;
	}

	resume_upload = stream_context->upload_paused &&
			worker_has_upload_data(stream_context);
	resume_receive = stream_context->receive_paused &&
			mws_source_audio_buffer_free_bytes(
					&stream_context->source_audio_buffer) >=
			stream_context->receive_pause_required_bytes;
	if (!resume_upload && !resume_receive) {
		return SWITCH_TRUE;
	}

	if (resume_upload) {
		stream_context->upload_paused = SWITCH_FALSE;
	}
	if (resume_receive) {
		stream_context->receive_paused = SWITCH_FALSE;
		stream_context->receive_pause_required_bytes = 0;
	}
	if (stream_context->upload_paused) {
		pause_action |= CURLPAUSE_SEND;
	}
	if (stream_context->receive_paused) {
		pause_action |= CURLPAUSE_RECV;
	}
	result = curl_easy_pause(stream_context->easy, pause_action);
	if (result == CURLE_OK) {
		return SWITCH_TRUE;
	}

	return worker_finish_transport(multi, stream_context, result);
}

static switch_bool_t worker_collect_transport_results(CURLM *multi)
{
	CURLMsg *message;
	int messages_remaining;

	while ((message = curl_multi_info_read(
				multi,
				&messages_remaining)) != NULL) {
		mws_stream_t *stream_context = NULL;

		if (message->msg != CURLMSG_DONE || !message->easy_handle) {
			continue;
		}

		if (curl_easy_getinfo(
					message->easy_handle,
					CURLINFO_PRIVATE,
					&stream_context) != CURLE_OK ||
				!stream_context ||
				stream_context->easy != message->easy_handle) {
			CURLMcode result = curl_multi_remove_handle(
					multi,
					message->easy_handle);

			if (result != CURLM_OK) {
				switch_log_printf(
						SWITCH_CHANNEL_LOG,
						SWITCH_LOG_CRIT,
						"Could not remove unrecognized WebSocket "
						"easy handle: %s\n",
						curl_multi_strerror(result));
				return SWITCH_FALSE;
			}
			curl_easy_cleanup(message->easy_handle);
			continue;
		}

		if (!worker_finish_transport(
				multi,
				stream_context,
				message->data.result)) {
			return SWITCH_FALSE;
		}
	}

	return SWITCH_TRUE;
}

static switch_bool_t worker_drive_transports(
		CURLM *multi,
		mws_stream_t *worker_streams)
{
	CURLMcode result;
	int running_handles;

	result = curl_multi_perform(multi, &running_handles);
	if (result != CURLM_OK) {
		mws_stream_t *stream_context;

		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"WebSocket multi transfer failed: %s\n",
				curl_multi_strerror(result));

		for (stream_context = worker_streams; stream_context;
				stream_context = stream_context->worker_next) {
			if (!stream_context->easy) {
				continue;
			}

			stream_context->multi_result = result;
			stream_context->transport_terminal_state = MWS_TRANSPORT_FAILED;
			worker_set_transport_state(
					stream_context,
					MWS_TRANSPORT_FAILED);
			worker_request_automatic_stream_cleanup(
					stream_context,
					curl_multi_strerror(result));
		}
		return SWITCH_FALSE;
	}

	if (!worker_collect_transport_results(multi)) {
		mws_stream_t *stream_context;

		for (stream_context = worker_streams; stream_context;
				stream_context = stream_context->worker_next) {
			if (!stream_context->easy) {
				continue;
			}

			stream_context->transport_terminal_state = MWS_TRANSPORT_FAILED;
			worker_set_transport_state(
					stream_context,
					MWS_TRANSPORT_FAILED);
			worker_request_automatic_stream_cleanup(
					stream_context,
					"WebSocket transport result collection failed");
		}
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

static void worker_add_stream(
		CURLM *multi,
		mws_stream_t **worker_streams,
		mws_stream_t *stream_context)
{
	mws_stream_t *current;

	for (current = *worker_streams; current; current = current->worker_next) {
		if (current == stream_context) {
			worker_acknowledge_add(stream_context);
			return;
		}
	}

	stream_context->worker_next = *worker_streams;
	*worker_streams = stream_context;
	worker_reset_connection_attempt(stream_context);
	worker_start_transport(multi, stream_context);
	worker_acknowledge_add(stream_context);
}

static switch_bool_t worker_finish_stream_removal(
		CURLM *multi,
		mws_stream_t **stream_link,
		mws_stream_t *stream_context,
		switch_bool_t normal_close_timed_out)
{
	const char *error_message;

	worker_reset_pcm_state(stream_context);
	if (worker_cleanup_easy(multi, stream_context) != CURLM_OK) {
		return SWITCH_FALSE;
	}

	if (stream_context->transport_terminal_state != MWS_TRANSPORT_FAILED) {
		stream_context->transport_terminal_state = MWS_TRANSPORT_CLOSED;
	}
	worker_set_transport_state(
			stream_context,
			MWS_TRANSPORT_CLOSED);

	if (normal_close_timed_out) {
		error_message = "WebSocket close handshake timed out";
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"WebSocket close handshake timed out; "
				"closing transport: uuid=%s\n",
				stream_context->session_uuid);
	} else {
		error_message = stream_context->curl_error[0] ?
				stream_context->curl_error :
				curl_easy_strerror(stream_context->curl_result);
	}
	worker_report_terminal_result(stream_context, error_message);

	*stream_link = stream_context->worker_next;
	stream_context->worker_next = NULL;

	worker_acknowledge_remove(stream_context);

	return SWITCH_TRUE;
}

static switch_bool_t worker_begin_stream_removal(
		CURLM *multi,
		mws_stream_t **worker_streams,
		mws_stream_t *stream_context)
{
	mws_stream_t **current;

	for (current = worker_streams; *current;
			current = &(*current)->worker_next) {
		if (*current != stream_context) {
			continue;
		}

		if (stream_context->worker_removal_requested) {
			return SWITCH_TRUE;
		}
		stream_context->worker_removal_requested = SWITCH_TRUE;

		if (stream_context->easy &&
				stream_context->transport_state == MWS_TRANSPORT_OPEN) {
			if (!stream_context->normal_close_requested) {
				stream_context->close_frame_payload[0] =
						(uint8_t) (MWS_WEBSOCKET_NORMAL_CLOSE_CODE >> 8);
				stream_context->close_frame_payload[1] =
						(uint8_t) MWS_WEBSOCKET_NORMAL_CLOSE_CODE;
				stream_context->close_frame_payload_length_bytes = 2;
				stream_context->normal_close_requested = SWITCH_TRUE;
				stream_context->normal_close_deadline = switch_time_now() +
						MWS_CLOSE_HANDSHAKE_TIMEOUT_US;
				switch_log_printf(
						SWITCH_CHANNEL_LOG,
						SWITCH_LOG_DEBUG,
						"WebSocket normal close requested: uuid=%s\n",
						stream_context->session_uuid);
			}
			return SWITCH_TRUE;
		}

		return worker_finish_stream_removal(
				multi,
				current,
				stream_context,
				SWITCH_FALSE);
	}

	worker_acknowledge_remove(stream_context);
	return SWITCH_TRUE;
}

static switch_bool_t worker_finish_pending_stream_removals(
		CURLM *multi,
		mws_stream_t **worker_streams)
{
	mws_stream_t **current = worker_streams;
	switch_time_t now = switch_time_now();

	while (*current) {
		mws_stream_t *stream_context = *current;
		switch_bool_t normal_close_timed_out;

		if (!stream_context->worker_removal_requested) {
			current = &stream_context->worker_next;
			continue;
		}

		normal_close_timed_out = stream_context->easy &&
				now >= stream_context->normal_close_deadline;
		if (stream_context->easy && !normal_close_timed_out) {
			current = &stream_context->worker_next;
			continue;
		}

		if (!worker_finish_stream_removal(
				multi,
				current,
				stream_context,
				normal_close_timed_out)) {
			return SWITCH_FALSE;
		}
	}

	return SWITCH_TRUE;
}

static switch_bool_t worker_expire_peer_close_handshakes(
		CURLM *multi,
		mws_stream_t *worker_streams)
{
	mws_stream_t *stream_context;
	switch_time_t now = switch_time_now();

	for (stream_context = worker_streams; stream_context;
			stream_context = stream_context->worker_next) {
		if (!stream_context->normal_close_requested ||
				stream_context->worker_removal_requested ||
				!stream_context->easy ||
				now < stream_context->normal_close_deadline) {
			continue;
		}

		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"WebSocket peer close handshake timed out; "
				"closing transport: uuid=%s\n",
				stream_context->session_uuid);
		worker_reset_pcm_state(stream_context);
		if (worker_cleanup_easy(multi, stream_context) != CURLM_OK) {
			return SWITCH_FALSE;
		}
		stream_context->transport_terminal_state = MWS_TRANSPORT_CLOSED;
		worker_set_transport_state(
				stream_context,
				MWS_TRANSPORT_CLOSED);
		worker_request_automatic_stream_cleanup(
				stream_context,
				"WebSocket peer close handshake timed out");
	}

	return SWITCH_TRUE;
}

static void worker_complete_clear_command(mws_stream_t *stream_context)
{
	uint64_t discarded_sample_count =
			worker_clear_websocket_audio(stream_context);

	switch_mutex_lock(stream_context->worker_state_mutex);
	stream_context->clear_command_result_sample_count =
			discarded_sample_count;
	stream_context->clear_command_complete = SWITCH_TRUE;
	switch_thread_cond_broadcast(stream_context->worker_state_condition);
	switch_mutex_unlock(stream_context->worker_state_mutex);
}

static void worker_fire_due_statistics_events(mws_stream_t *worker_streams)
{
	mws_stream_t *stream_context;
	switch_time_t now = switch_time_now();

	for (stream_context = worker_streams; stream_context;
			stream_context = stream_context->worker_next) {
		if (stream_context->statistics_interval_ms <= 0 ||
				stream_context->transport_state != MWS_TRANSPORT_OPEN ||
				!stream_context->next_statistics_event_at ||
				now < stream_context->next_statistics_event_at) {
			continue;
		}

		stream_context->next_statistics_event_at = now +
				(switch_time_t) stream_context->statistics_interval_ms * 1000;
		worker_fire_statistics_event(stream_context);
	}
}

static void *SWITCH_THREAD_FUNC websocket_stream_worker(
		switch_thread_t *thread,
		void *data)
{
	mws_transport_worker_t *worker = (mws_transport_worker_t *) data;
	CURLM *multi = worker->multi;
	mws_stream_t *worker_streams = NULL;
	switch_bool_t running = SWITCH_TRUE;
	switch_bool_t cleanup_streams = SWITCH_FALSE;

	(void) thread;

	while (running) {
		mws_worker_command_t *command = NULL;
		void *queue_data = NULL;
		switch_status_t status;

		if (worker_streams) {
			status = switch_queue_pop_timeout(
					worker->commands,
					&queue_data,
					MWS_WORKER_POLL_INTERVAL_US);
		} else {
			status = switch_queue_pop(
					worker->commands,
					&queue_data);
		}

		if (status == SWITCH_STATUS_SUCCESS && queue_data) {
			command = (mws_worker_command_t *) queue_data;
			release_regular_worker_command_slot(worker, command);

			switch (command->type) {
			case MWS_WORKER_COMMAND_ADD:
				worker_add_stream(
						multi,
						&worker_streams,
						command->stream_context);
				break;
			case MWS_WORKER_COMMAND_REMOVE:
				if (!worker_begin_stream_removal(
						multi,
						&worker_streams,
						command->stream_context)) {
					cleanup_streams = SWITCH_TRUE;
					running = SWITCH_FALSE;
				}
				break;
			case MWS_WORKER_COMMAND_CLEAR:
				worker_complete_clear_command(command->stream_context);
				break;
			case MWS_WORKER_COMMAND_STOP:
				running = SWITCH_FALSE;
				break;
			default:
				break;
			}
		} else if (status == SWITCH_STATUS_TERM) {
			running = SWITCH_FALSE;
		}

		if (running) {
			mws_stream_t *stream_context;

			for (stream_context = worker_streams; stream_context;
					stream_context = stream_context->worker_next) {
				if (worker_apply_pending_playback_format(stream_context) !=
						SWITCH_STATUS_SUCCESS) {
					atomic_fetch_add_explicit(
							&stream_context->playback_invalid_frame_count,
							1,
							memory_order_relaxed);
				}
			}

			worker_start_due_reconnects(multi, worker_streams);

			if (!worker_drive_transports(multi, worker_streams)) {
				cleanup_streams = SWITCH_TRUE;
				running = SWITCH_FALSE;
				continue;
			}

			for (stream_context = worker_streams; stream_context;
					stream_context = stream_context->worker_next) {
				if (worker_process_playback_source(stream_context) !=
						SWITCH_STATUS_SUCCESS &&
						!worker_finish_transport(
							multi,
							stream_context,
							CURLE_WRITE_ERROR)) {
					cleanup_streams = SWITCH_TRUE;
					running = SWITCH_FALSE;
					break;
				}
			}
			if (!running) {
				continue;
			}

			for (stream_context = worker_streams; stream_context;
					stream_context = stream_context->worker_next) {
				if (!worker_resume_stream(multi, stream_context)) {
					cleanup_streams = SWITCH_TRUE;
					running = SWITCH_FALSE;
					break;
				}
			}
			if (running) {
				worker_fire_due_statistics_events(worker_streams);
			}

			if (running && !worker_expire_peer_close_handshakes(
					multi,
					worker_streams)) {
				cleanup_streams = SWITCH_TRUE;
				running = SWITCH_FALSE;
			}

			if (running && !worker_finish_pending_stream_removals(
					multi,
					&worker_streams)) {
				cleanup_streams = SWITCH_TRUE;
				running = SWITCH_FALSE;
			}
		}
	}

	switch_mutex_lock(worker->mutex);
	if (worker->cleanup_streams_on_stop) {
		cleanup_streams = SWITCH_TRUE;
	}
	worker->stopping = SWITCH_TRUE;
	switch_mutex_unlock(worker->mutex);

	{
		mws_stream_t *stream_context;

		for (stream_context = worker_streams; stream_context;
				stream_context = stream_context->worker_next) {
			if (cleanup_streams) {
				stream_context->transport_terminal_state =
						MWS_TRANSPORT_FAILED;
				worker_set_transport_state(
						stream_context,
						MWS_TRANSPORT_FAILED);
				worker_request_automatic_stream_cleanup(
						stream_context,
						"WebSocket transport worker stopped unexpectedly");
			}
			worker_reset_pcm_state(stream_context);
			(void) worker_cleanup_easy(multi, stream_context);
		}
	}

	curl_multi_cleanup(multi);
	worker->multi = NULL;

	while (worker_streams) {
		mws_stream_t *stream_context = worker_streams;

		worker_reset_pcm_state(stream_context);
		if (stream_context->easy) {
			stream_context->easy_added = SWITCH_FALSE;
			curl_easy_cleanup(stream_context->easy);
			stream_context->easy = NULL;
			curl_slist_free_all(stream_context->curl_request_headers);
			stream_context->curl_request_headers = NULL;
			stream_context->upload_paused = SWITCH_FALSE;
			stream_context->receive_paused = SWITCH_FALSE;
			stream_context->receive_pause_required_bytes = 0;
		}
		if (!cleanup_streams &&
				stream_context->transport_terminal_state == MWS_TRANSPORT_NEW) {
			stream_context->transport_terminal_state = MWS_TRANSPORT_CLOSED;
		}
		if (!cleanup_streams) {
			worker_set_transport_state(
					stream_context,
					MWS_TRANSPORT_CLOSED);
		}
		worker_streams = stream_context->worker_next;
		stream_context->worker_next = NULL;
		worker_acknowledge_remove(stream_context);
	}

	switch_mutex_lock(worker->mutex);
	worker->running = SWITCH_FALSE;
	worker->stopping = SWITCH_TRUE;
	switch_mutex_unlock(worker->mutex);

	return NULL;
}

static switch_status_t start_transport_worker(
		mws_transport_worker_t *worker,
		uint32_t index,
		switch_memory_pool_t *pool)
{
	CURLM *multi;
	switch_threadattr_t *thread_attributes;
	switch_status_t status;

	if (switch_mutex_init(
				&worker->mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(
				&worker->shutdown_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_queue_create(
				&worker->commands,
				MWS_WORKER_COMMAND_QUEUE_CAPACITY,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_threadattr_create(&thread_attributes, pool) !=
			SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	multi = curl_multi_init();
	if (!multi) {
		return SWITCH_STATUS_FALSE;
	}

	switch_threadattr_detach_set(thread_attributes, 0);
	switch_threadattr_stacksize_set(
			thread_attributes,
			SWITCH_THREAD_STACKSIZE);

	worker->multi = multi;
	worker->index = index;
	worker->stop_command.type = MWS_WORKER_COMMAND_STOP;
	worker->stop_command.stream_context = NULL;
	switch_mutex_lock(worker->mutex);
	worker->running = SWITCH_TRUE;
	worker->stopping = SWITCH_FALSE;
	worker->stop_queued = SWITCH_FALSE;
	worker->cleanup_streams_on_stop = SWITCH_FALSE;
	worker->regular_commands_queued = 0;
	worker->assigned_streams = 0;
	switch_mutex_unlock(worker->mutex);

	status = switch_thread_create(
			&worker->thread,
			thread_attributes,
			websocket_stream_worker,
			worker,
			pool);

	if (status != SWITCH_STATUS_SUCCESS) {
		curl_multi_cleanup(multi);
		worker->multi = NULL;
		switch_mutex_lock(worker->mutex);
		worker->running = SWITCH_FALSE;
		worker->stopping = SWITCH_TRUE;
		worker->thread = NULL;
		switch_mutex_unlock(worker->mutex);
	}

	return status;
}

static uint32_t configured_transport_worker_count(void)
{
	uint32_t cpu_count;
	uint32_t worker_count;

	if (globals.configured_transport_workers > 0) {
		return (uint32_t) globals.configured_transport_workers;
	}

	cpu_count = switch_core_cpu_count();
	worker_count = cpu_count / 2U;
	if (!worker_count) {
		worker_count = 1;
	}
	if (worker_count > MWS_AUTOMATIC_MAXIMUM_TRANSPORT_WORKERS) {
		worker_count = MWS_AUTOMATIC_MAXIMUM_TRANSPORT_WORKERS;
	}

	return worker_count;
}

static switch_status_t start_transport_workers(switch_memory_pool_t *pool)
{
	uint32_t worker_count = configured_transport_worker_count();
	uint32_t index;

	if (switch_mutex_init(
				&globals.worker_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS ||
			switch_mutex_init(
				&globals.transport_assignment_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	globals.transport_workers = switch_core_alloc(
			pool,
			sizeof(*globals.transport_workers) * worker_count);
	if (!globals.transport_workers) {
		return SWITCH_STATUS_MEMERR;
	}
	globals.transport_worker_count = worker_count;
	globals.next_transport_worker = 0;
	globals.transport_workers_stopping = SWITCH_FALSE;

	switch_mutex_lock(globals.worker_mutex);
	globals.connecting_streams = 0;
	globals.open_streams = 0;
	globals.reconnecting_streams = 0;
	globals.failed_streams = 0;
	switch_mutex_unlock(globals.worker_mutex);

	for (index = 0; index < worker_count; index++) {
		if (start_transport_worker(
				&globals.transport_workers[index],
				index,
				pool) != SWITCH_STATUS_SUCCESS) {
			uint32_t started;

			switch_mutex_lock(globals.transport_assignment_mutex);
			globals.transport_workers_stopping = SWITCH_TRUE;
			switch_mutex_unlock(globals.transport_assignment_mutex);
			for (started = 0; started < index; started++) {
				stop_transport_worker_for_lifetime_safety(
						&globals.transport_workers[started],
						SWITCH_FALSE);
			}
			return SWITCH_STATUS_FALSE;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t start_stream_worker(mws_stream_t *stream_context)
{
	mws_transport_worker_t *worker = reserve_transport_worker();
	switch_status_t status;

	if (!worker) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(stream_context->worker_state_mutex);
	if (atomic_load_explicit(
			&stream_context->shutdown_requested,
			memory_order_acquire)) {
		switch_mutex_unlock(stream_context->worker_state_mutex);
		release_transport_worker_reservation(worker);
		return SWITCH_STATUS_FALSE;
	}
	stream_context->worker_add_queued = SWITCH_TRUE;
	stream_context->worker_remove_queued = SWITCH_FALSE;
	stream_context->worker_attached = SWITCH_FALSE;
	stream_context->worker_removed = SWITCH_FALSE;
	stream_context->transport_worker = worker;
	stream_context->worker_assignment_counted = SWITCH_TRUE;
	switch_mutex_unlock(stream_context->worker_state_mutex);

	status = push_worker_command(worker, &stream_context->worker_add_command);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(stream_context->worker_state_mutex);
		stream_context->worker_add_queued = SWITCH_FALSE;
		stream_context->worker_assignment_counted = SWITCH_FALSE;
		switch_mutex_unlock(stream_context->worker_state_mutex);
		release_transport_worker_reservation(worker);
		return status;
	}

	switch_mutex_lock(stream_context->worker_state_mutex);
	while (!stream_context->worker_attached &&
			!stream_context->worker_removed &&
			!atomic_load_explicit(
					&stream_context->shutdown_requested,
					memory_order_acquire)) {
		switch_thread_cond_timedwait(
				stream_context->worker_state_condition,
				stream_context->worker_state_mutex,
				MWS_WORKER_ACK_POLL_INTERVAL_US);
		if (stream_context->worker_attached ||
				stream_context->worker_removed ||
				atomic_load_explicit(
						&stream_context->shutdown_requested,
						memory_order_acquire)) {
			continue;
		}
		if (!transport_worker_is_running(worker)) {
			switch_bool_t release_assignment;

			stream_context->worker_add_queued = SWITCH_FALSE;
			stream_context->worker_removed = SWITCH_TRUE;
			release_assignment =
					worker_claim_assignment_locked(stream_context);
			switch_mutex_unlock(stream_context->worker_state_mutex);
			if (release_assignment) {
				release_transport_worker_reservation(worker);
			}
			return SWITCH_STATUS_FALSE;
		}
	}
	if (stream_context->worker_removed ||
			atomic_load_explicit(
					&stream_context->shutdown_requested,
					memory_order_acquire)) {
		switch_mutex_unlock(stream_context->worker_state_mutex);
		return SWITCH_STATUS_FALSE;
	}
	switch_mutex_unlock(stream_context->worker_state_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t request_stream_worker_stop(
		mws_stream_t *stream_context)
{
	mws_transport_worker_t *worker = stream_context->transport_worker;
	switch_status_t status;

	switch_mutex_lock(stream_context->api_command_mutex);
	stream_context->api_command_active = SWITCH_FALSE;
	switch_mutex_unlock(stream_context->api_command_mutex);

	switch_mutex_lock(stream_context->worker_state_mutex);
	if (!stream_context->worker_add_queued ||
			stream_context->worker_removed ||
			stream_context->worker_remove_queued) {
		switch_mutex_unlock(stream_context->worker_state_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	stream_context->worker_remove_queued = SWITCH_TRUE;
	switch_mutex_unlock(stream_context->worker_state_mutex);

	if (!worker) {
		return SWITCH_STATUS_FALSE;
	}

	status = push_worker_command(worker, &stream_context->worker_remove_command);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(stream_context->worker_state_mutex);
		stream_context->worker_remove_queued = SWITCH_FALSE;
		switch_thread_cond_broadcast(stream_context->worker_state_condition);
		switch_mutex_unlock(stream_context->worker_state_mutex);

		stop_transport_worker_for_lifetime_safety(worker, SWITCH_TRUE);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t wait_for_stream_worker_stop(
		mws_stream_t *stream_context)
{
	mws_transport_worker_t *worker = stream_context->transport_worker;
	switch_bool_t release_assignment = SWITCH_FALSE;

	switch_mutex_lock(stream_context->worker_state_mutex);
	while (!stream_context->worker_removed) {
		switch_thread_cond_timedwait(
				stream_context->worker_state_condition,
				stream_context->worker_state_mutex,
				MWS_WORKER_ACK_POLL_INTERVAL_US);
		if (!worker || !transport_worker_is_running(worker)) {
			stream_context->worker_attached = SWITCH_FALSE;
			stream_context->worker_removed = SWITCH_TRUE;
			release_assignment =
					worker_claim_assignment_locked(stream_context);
			break;
		}
	}
	switch_mutex_unlock(stream_context->worker_state_mutex);
	if (release_assignment) {
		release_transport_worker_reservation(worker);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t stop_stream_worker(mws_stream_t *stream_context)
{
	switch_status_t status = request_stream_worker_stop(stream_context);

	if (status != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	return wait_for_stream_worker_stop(stream_context);
}

static switch_status_t clear_stream_worker(
		mws_stream_t *stream_context,
		uint64_t *discarded_sample_count)
{
	mws_transport_worker_t *worker = stream_context->transport_worker;
	switch_status_t status;

	if (!worker) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(stream_context->worker_state_mutex);
	if (atomic_load_explicit(
			&stream_context->shutdown_requested,
			memory_order_acquire) ||
			stream_context->clear_command_queued) {
		switch_mutex_unlock(stream_context->worker_state_mutex);
		return SWITCH_STATUS_FALSE;
	}
	stream_context->clear_command_queued = SWITCH_TRUE;
	stream_context->clear_command_complete = SWITCH_FALSE;
	switch_mutex_unlock(stream_context->worker_state_mutex);

	status = push_worker_command(worker, &stream_context->worker_clear_command);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_mutex_lock(stream_context->worker_state_mutex);
		stream_context->clear_command_queued = SWITCH_FALSE;
		switch_mutex_unlock(stream_context->worker_state_mutex);
		return status;
	}

	switch_mutex_lock(stream_context->worker_state_mutex);
	while (!stream_context->clear_command_complete) {
		switch_thread_cond_timedwait(
				stream_context->worker_state_condition,
				stream_context->worker_state_mutex,
				MWS_WORKER_ACK_POLL_INTERVAL_US);
		if (!transport_worker_is_running(worker)) {
			stream_context->clear_command_queued = SWITCH_FALSE;
			switch_mutex_unlock(stream_context->worker_state_mutex);
			return SWITCH_STATUS_FALSE;
		}
	}
	if (discarded_sample_count) {
		*discarded_sample_count =
				stream_context->clear_command_result_sample_count;
	}
	stream_context->clear_command_queued = SWITCH_FALSE;
	stream_context->clear_command_complete = SWITCH_FALSE;
	switch_mutex_unlock(stream_context->worker_state_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t stop_transport_worker(mws_transport_worker_t *worker)
{
	switch_thread_t *worker_thread;
	switch_status_t thread_status = SWITCH_STATUS_SUCCESS;
	switch_status_t status;

	switch_mutex_lock(worker->shutdown_mutex);
	switch_mutex_lock(worker->mutex);
	worker_thread = worker->thread;
	if (!worker_thread) {
		switch_mutex_unlock(worker->mutex);
		switch_mutex_unlock(worker->shutdown_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	worker->stopping = SWITCH_TRUE;
	switch_mutex_unlock(worker->mutex);

	status = push_worker_command(worker, &worker->stop_command);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_queue_term(worker->commands);
	}

	status = switch_thread_join(&thread_status, worker_thread);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_mutex_unlock(worker->shutdown_mutex);
		return status;
	}

	switch_mutex_lock(worker->mutex);
	worker->thread = NULL;
	worker->running = SWITCH_FALSE;
	worker->stop_queued = SWITCH_FALSE;
	switch_mutex_unlock(worker->mutex);
	switch_mutex_unlock(worker->shutdown_mutex);

	return thread_status;
}

static switch_status_t stop_transport_workers(void)
{
	switch_status_t result = SWITCH_STATUS_SUCCESS;
	uint32_t index;

	switch_mutex_lock(globals.transport_assignment_mutex);
	globals.transport_workers_stopping = SWITCH_TRUE;
	switch_mutex_unlock(globals.transport_assignment_mutex);

	for (index = 0; index < globals.transport_worker_count; index++) {
		mws_transport_worker_t *worker =
				&globals.transport_workers[index];
		switch_status_t status = stop_transport_worker(worker);

		if (status != SWITCH_STATUS_SUCCESS &&
				result == SWITCH_STATUS_SUCCESS) {
			result = status;
		}
		if (status != SWITCH_STATUS_SUCCESS) {
			stop_transport_worker_for_lifetime_safety(
					worker,
					SWITCH_FALSE);
		}
	}

	return result;
}

static void stop_transport_worker_for_lifetime_safety(
		mws_transport_worker_t *worker,
		switch_bool_t cleanup_streams)
{
	if (cleanup_streams) {
		switch_mutex_lock(worker->mutex);
		worker->cleanup_streams_on_stop = SWITCH_TRUE;
		switch_mutex_unlock(worker->mutex);
	}

	for (;;) {
		switch_bool_t has_thread;

		switch_mutex_lock(worker->mutex);
		has_thread = worker->thread != NULL;
		switch_mutex_unlock(worker->mutex);
		if (!has_thread) {
			break;
		}

		if (stop_transport_worker(worker) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(
					SWITCH_CHANNEL_LOG,
					SWITCH_LOG_CRIT,
					"Could not join WebSocket transport worker %u; retrying\n",
					worker->index);
			switch_yield(1000);
		}
	}
}

static void register_stream(mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.streams_mutex);

	switch_assert(stream_context->admission_reserved);
	stream_context->next = globals.streams;
	globals.streams = stream_context;
	globals.active_streams++;

	switch_mutex_unlock(globals.streams_mutex);
}

static switch_bool_t claim_stream_teardown(mws_stream_t *stream_context)
{
	switch_bool_t claimed = SWITCH_FALSE;

	switch_mutex_lock(stream_context->worker_state_mutex);
	if (!atomic_load_explicit(
			&stream_context->teardown_claimed,
			memory_order_acquire)) {
		atomic_store_explicit(
				&stream_context->teardown_claimed,
				true,
				memory_order_release);
		claimed = SWITCH_TRUE;
	}
	switch_mutex_unlock(stream_context->worker_state_mutex);

	return claimed;
}

static switch_bool_t complete_stream_startup(mws_stream_t *stream_context)
{
	switch_bool_t committed;

	switch_mutex_lock(stream_context->worker_state_mutex);
	committed = !atomic_load_explicit(
			&stream_context->teardown_claimed,
			memory_order_acquire);
	atomic_store_explicit(
			&stream_context->startup_complete,
			true,
			memory_order_release);
	switch_thread_cond_broadcast(stream_context->worker_state_condition);
	switch_mutex_unlock(stream_context->worker_state_mutex);

	return committed;
}

static void wait_for_stream_startup(mws_stream_t *stream_context)
{
	switch_mutex_lock(stream_context->worker_state_mutex);
	while (!atomic_load_explicit(
			&stream_context->startup_complete,
			memory_order_acquire)) {
		switch_thread_cond_wait(
				stream_context->worker_state_condition,
				stream_context->worker_state_mutex);
	}
	switch_mutex_unlock(stream_context->worker_state_mutex);
}

static mws_channel_t *get_or_create_channel_context_locked(
		switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	mws_channel_t *channel_context = switch_channel_get_private(
			channel,
			MWS_CHANNEL_CONTEXT_PRIVATE);

	if (channel_context) {
		return channel_context;
	}

	channel_context = switch_core_session_alloc(
			session,
			sizeof(*channel_context));
	if (!channel_context ||
			switch_mutex_init(
					&channel_context->mutex,
					SWITCH_MUTEX_UNNESTED,
					switch_core_session_get_pool(session)) !=
			SWITCH_STATUS_SUCCESS ||
			switch_thread_cond_create(
					&channel_context->condition,
					switch_core_session_get_pool(session)) !=
			SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	channel_context->session_uuid = switch_core_session_strdup(
			session,
			switch_core_session_get_uuid(session));
	if (!channel_context->session_uuid) {
		return NULL;
	}
	mws_media_callback_clock_init(&channel_context->read_playback_clock);
	mws_media_callback_clock_init(&channel_context->write_playback_clock);

	switch_channel_set_private(
			channel,
			MWS_CHANNEL_CONTEXT_PRIVATE,
			channel_context);

	return channel_context;
}

static mws_admission_result_t reserve_stream_admission(
		switch_core_session_t *session,
		mws_channel_t **reserved_channel)
{
	mws_channel_t *channel_context;
	mws_admission_result_t result = MWS_ADMISSION_OK;
	uint32_t maximum_streams;
	uint32_t maximum_streams_per_channel;

	switch_mutex_lock(globals.config_mutex);
	maximum_streams = (uint32_t) globals.maximum_streams;
	maximum_streams_per_channel =
			(uint32_t) globals.maximum_streams_per_channel;
	switch_mutex_unlock(globals.config_mutex);

	switch_mutex_lock(globals.streams_mutex);
	if (globals.reserved_streams >= maximum_streams) {
		result = MWS_ADMISSION_GLOBAL_LIMIT;
		goto done;
	}

	channel_context = get_or_create_channel_context_locked(session);
	if (!channel_context) {
		result = MWS_ADMISSION_NO_MEMORY;
		goto done;
	}
	result = mws_stream_admission_reserve(
			&globals.reserved_streams,
			&channel_context->reserved_streams,
			maximum_streams,
			maximum_streams_per_channel);
	if (result == MWS_ADMISSION_OK) {
		*reserved_channel = channel_context;
	}

done:
	switch_mutex_unlock(globals.streams_mutex);
	return result;
}

static void release_channel_admission(mws_channel_t *channel_context)
{
	if (!channel_context) {
		return;
	}

	switch_mutex_lock(globals.streams_mutex);
	switch_assert(globals.reserved_streams > 0);
	switch_assert(channel_context->reserved_streams > 0);
	mws_stream_admission_release(
			&globals.reserved_streams,
			&channel_context->reserved_streams);
	switch_mutex_unlock(globals.streams_mutex);
}

static void release_stream_admission(mws_stream_t *stream_context)
{
	if (!stream_context || !stream_context->admission_reserved) {
		return;
	}

	switch_mutex_lock(globals.streams_mutex);
	if (stream_context->admission_reserved) {
		switch_assert(globals.reserved_streams > 0);
		switch_assert(stream_context->admission_channel_context);
		switch_assert(
				stream_context->admission_channel_context->reserved_streams > 0);
		mws_stream_admission_release(
				&globals.reserved_streams,
				&stream_context->admission_channel_context->reserved_streams);
		stream_context->admission_reserved = SWITCH_FALSE;
		stream_context->admission_channel_context = NULL;
	}
	switch_mutex_unlock(globals.streams_mutex);
}

static void release_stream_channel_admission(mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.streams_mutex);
	if (stream_context->admission_reserved &&
			stream_context->admission_channel_context) {
		switch_assert(
				stream_context->admission_channel_context->reserved_streams > 0);
		stream_context->admission_channel_context->reserved_streams--;
		stream_context->admission_channel_context = NULL;
	}
	switch_mutex_unlock(globals.streams_mutex);
}

static void keep_stream_slot_until_cleanup(
		mws_stream_t *stream_context)
{
	switch_mutex_lock(globals.streams_mutex);
	switch_assert(stream_context->admission_reserved);
	stream_context->cleanup_holds_stream_slot = SWITCH_TRUE;
	switch_mutex_unlock(globals.streams_mutex);
}

static void defer_claimed_stream_cleanup(mws_stream_t *stream_context)
{
	retain_stream_resource_borrow(stream_context);
	defer_claimed_stream_cleanup_with_borrow(stream_context);
}

static void defer_claimed_stream_cleanup_with_borrow(
		mws_stream_t *stream_context)
{
	queue_detached_stream_cleanup(stream_context);
}

static void reopen_empty_channel_context(mws_channel_t *channel_context)
{
	switch_mutex_lock(channel_context->mutex);
	if (!channel_context->bug &&
			!channel_context->streams &&
			!channel_context->media_bug_add_in_progress) {
		mws_media_callback_clock_init(&channel_context->read_playback_clock);
		mws_media_callback_clock_init(&channel_context->write_playback_clock);
		channel_context->closing = SWITCH_FALSE;
	}
	switch_mutex_unlock(channel_context->mutex);
}

static switch_status_t attach_stream_to_channel(
		mws_channel_t *channel_context,
		mws_stream_t *stream_context)
{
	switch_mutex_lock(channel_context->mutex);
	if (channel_context->closing) {
		switch_mutex_unlock(channel_context->mutex);
		return SWITCH_STATUS_FALSE;
	}

	stream_context->channel_context = channel_context;
	stream_context->channel_next = channel_context->streams;
	channel_context->streams = stream_context;
	switch_mutex_unlock(channel_context->mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t detach_stream_from_channel(
		mws_stream_t *stream_context)
{
	mws_channel_t *channel_context = stream_context->channel_context;
	mws_stream_t **current;
	switch_bool_t removed = SWITCH_FALSE;
	switch_bool_t last_stream = SWITCH_FALSE;

	if (!channel_context) {
		return SWITCH_FALSE;
	}

	switch_mutex_lock(channel_context->mutex);
	for (current = &channel_context->streams; *current;
			current = &(*current)->channel_next) {
		if (*current == stream_context) {
			*current = stream_context->channel_next;
			stream_context->channel_next = NULL;
			stream_context->channel_context = NULL;
			removed = SWITCH_TRUE;
			break;
		}
	}
	if (removed && !channel_context->streams) {
		channel_context->closing = SWITCH_TRUE;
		last_stream = SWITCH_TRUE;
	}
	switch_mutex_unlock(channel_context->mutex);

	return last_stream;
}

static switch_bool_t stream_is_registered(mws_stream_t *stream_context)
{
	mws_stream_t *current;
	switch_bool_t registered = SWITCH_FALSE;

	switch_mutex_lock(globals.streams_mutex);
	for (current = globals.streams; current; current = current->next) {
		if (current == stream_context) {
			registered = SWITCH_TRUE;
			break;
		}
	}
	switch_mutex_unlock(globals.streams_mutex);

	return registered;
}

static switch_status_t collect_registered_streams(
		const char *session_uuid,
		const char *stream_id,
		mws_stream_t ***stream_contexts,
		switch_size_t *stream_count)
{
	mws_stream_t *current;
	mws_stream_t **matches = NULL;
	switch_size_t capacity = 0;
	switch_size_t index = 0;

	*stream_contexts = NULL;
	*stream_count = 0;

	switch_mutex_lock(globals.streams_mutex);
	for (current = globals.streams; current; current = current->next) {
		if (!strcmp(current->session_uuid, session_uuid) &&
				(zstr(stream_id) || !strcmp(current->stream_id, stream_id))) {
			capacity++;
		}
	}
	if (capacity) {
		matches = calloc(capacity, sizeof(*matches));
	}
	if (capacity && !matches) {
		switch_mutex_unlock(globals.streams_mutex);
		return SWITCH_STATUS_MEMERR;
	}
	for (current = globals.streams; current; current = current->next) {
		if (!strcmp(current->session_uuid, session_uuid) &&
				(zstr(stream_id) || !strcmp(current->stream_id, stream_id)) &&
				atomic_load_explicit(
						&current->startup_complete,
						memory_order_acquire)) {
			current->resource_borrow_count++;
			matches[index++] = current;
		}
	}
	switch_mutex_unlock(globals.streams_mutex);

	*stream_contexts = matches;
	*stream_count = index;
	return SWITCH_STATUS_SUCCESS;
}

static void release_registered_streams(
		mws_stream_t ***stream_contexts,
		switch_size_t stream_count)
{
	mws_stream_t **matches;
	switch_size_t index;

	if (!stream_contexts) {
		return;
	}

	matches = *stream_contexts;
	*stream_contexts = NULL;
	for (index = 0; index < stream_count; index++) {
		release_stream_resource_borrow(matches[index]);
	}
	switch_safe_free(matches);
}

static void snapshot_stream_statistics(
		mws_stream_t *stream_context,
		mws_stream_statistics_t *statistics)
{
	switch_time_t now = switch_time_now();
	uint64_t playback_rate_limit_updates;
	uint32_t playback_sample_rate;
	mws_captured_audio_buffer_statistics_t captured_audio_statistics;
	mws_playback_audio_resampler_statistics_t playback_resampler_statistics;
	mws_playback_timing_statistics_t playback_timing_statistics;

	memset(statistics, 0, sizeof(*statistics));

	switch_mutex_lock(globals.worker_mutex);
	statistics->transport_state = stream_context->transport_state;
	statistics->consecutive_reconnect_attempts =
			stream_context->consecutive_reconnect_attempts;
	statistics->reconnect_attempt_count =
			stream_context->reconnect_attempt_count;
	statistics->reconnect_success_count =
			stream_context->reconnect_success_count;
	if (stream_context->next_reconnect_at > now) {
		statistics->reconnect_in_ms = (uint64_t) (
				(stream_context->next_reconnect_at - now + 999) /
				1000);
	}
	switch_mutex_unlock(globals.worker_mutex);

	mws_captured_audio_buffer_snapshot(
			&stream_context->captured_audio_buffer,
			&captured_audio_statistics);
	statistics->captured_queued_bytes = captured_audio_statistics.queued_bytes;
	statistics->captured_capacity_bytes =
			captured_audio_statistics.capacity_bytes;
	statistics->fallback_queued_bytes =
			captured_audio_statistics.fallback_queued_bytes;
	statistics->captured_sample_rate = captured_audio_statistics.sample_rate;
	statistics->captured_channels = captured_audio_statistics.channels;
	statistics->captured_format_generation =
			captured_audio_statistics.format_generation;
	statistics->captured_invalid_frames =
			captured_audio_statistics.invalid_frames;
	statistics->captured_ring_lock_contentions =
			captured_audio_statistics.ring_lock_contentions;
	statistics->captured_fallback_drops =
			captured_audio_statistics.fallback_drops;
	statistics->captured_fallback_stores =
			captured_audio_statistics.fallback_stores;
	statistics->captured_fallback_overwrites =
			captured_audio_statistics.fallback_overwrites;
	statistics->captured_fallback_overwritten_bytes =
			captured_audio_statistics.fallback_overwritten_bytes;
	statistics->captured_fallback_abandoned_bytes =
			captured_audio_statistics.fallback_abandoned_bytes;
	statistics->captured_stale_ring_discarded_bytes =
			captured_audio_statistics.stale_ring_discarded_bytes;

	statistics->playback_queued_bytes =
			mws_playback_audio_buffer_queued_bytes(
					&stream_context->playback_audio_buffer);
	statistics->playback_source_queued_bytes =
			mws_source_audio_buffer_queued_bytes(
					&stream_context->source_audio_buffer);
	statistics->playback_source_capacity_bytes =
			stream_context->source_audio_buffer.capacity_bytes;
	statistics->playback_capacity_bytes =
			mws_playback_audio_buffer_capacity_bytes(
					&stream_context->playback_audio_buffer);
	statistics->playback_peak_queued_bytes =
			atomic_load_explicit(
				&stream_context->playback_peak_queued_bytes,
				memory_order_relaxed);
	statistics->playback_state = atomic_load_explicit(
			&stream_context->playback_state,
			memory_order_acquire);
	playback_sample_rate = atomic_load_explicit(
			&stream_context->playback_sample_rate,
			memory_order_acquire);
	statistics->playback_queued_duration_us = playback_queued_duration_us(
			stream_context,
			playback_sample_rate);
	mws_playback_audio_resampler_snapshot(
			&stream_context->playback_audio_resampler,
			&playback_resampler_statistics);
	statistics->playback_rate_target_queue_us =
			playback_resampler_statistics.target_queue_us;
	statistics->playback_rate_filtered_queue_us =
			playback_resampler_statistics.filtered_queue_us;
	statistics->playback_rate_adjustment_ppm =
			playback_resampler_statistics.rate_adjustment_ppm;
	statistics->playback_rate_maximum_adjustment_ppm =
			playback_resampler_statistics.maximum_rate_adjustment_ppm;
	statistics->playback_rate_updates =
			playback_resampler_statistics.rate_update_count;
	playback_rate_limit_updates =
			playback_resampler_statistics.rate_limit_count;
	statistics->playback_rate_limit_duration_us =
			playback_rate_limit_updates *
			MWS_PLAYBACK_RATE_UPDATE_INTERVAL_US;

	statistics->captured_drop_events = captured_audio_statistics.drop_events;
	statistics->captured_dropped_bytes = captured_audio_statistics.dropped_bytes;
	statistics->playback_received_samples = atomic_load_explicit(
			&stream_context->playback_received_sample_count,
			memory_order_relaxed);
	statistics->playback_source_dropped_samples = atomic_load_explicit(
			&stream_context->playback_source_dropped_sample_count,
			memory_order_relaxed);
	statistics->playback_buffered_samples = atomic_load_explicit(
			&stream_context->playback_buffered_sample_count,
			memory_order_relaxed);
	statistics->playback_mixed_samples = atomic_load_explicit(
			&stream_context->playback_mixed_sample_count,
			memory_order_relaxed);
	statistics->playback_dropped_samples = atomic_load_explicit(
			&stream_context->playback_dropped_sample_count,
			memory_order_relaxed);
	statistics->playback_invalid_frames = atomic_load_explicit(
			&stream_context->playback_invalid_frame_count,
			memory_order_relaxed);
	mws_playback_timing_snapshot(
			&stream_context->playback_timing,
			(uint64_t) switch_mono_micro_time_now(),
			&playback_timing_statistics);
	statistics->playback_underflow_frames =
			playback_timing_statistics.underflow_transition_count;
	statistics->playback_callbacks = playback_timing_statistics.callback_count;
	statistics->playback_requested_samples =
			playback_timing_statistics.callback_requested_samples;
	statistics->playback_callback_intervals =
			playback_timing_statistics.callback_interval_count;
	statistics->playback_callback_interval_total_us =
			playback_timing_statistics.callback_interval_total_us;
	statistics->playback_callback_max_interval_us =
			playback_timing_statistics.callback_maximum_interval_us;
	statistics->playback_callback_max_lateness_us =
			playback_timing_statistics.callback_maximum_lateness_us;
	statistics->playback_callback_gap_intervals =
			playback_timing_statistics.callback_gap_interval_count;
	statistics->playback_module_dispatch_max_delay_us =
			playback_timing_statistics.module_dispatch_maximum_delay_us;
	statistics->playback_underflow_with_source_audio_count =
			playback_timing_statistics.underflow_with_source_audio_count;
	statistics->playback_observation_snapshot_misses =
			playback_timing_statistics.observation_snapshot_miss_count;
	statistics->playback_websocket_audio_seen =
			playback_timing_statistics.websocket_audio_seen;
	statistics->playback_buffer_write_seen =
			playback_timing_statistics.playback_buffer_write_seen;
	statistics->playback_worker_visit_seen =
			playback_timing_statistics.worker_visit_seen;
	statistics->playback_websocket_audio_age_us =
			playback_timing_statistics.websocket_audio_age_us;
	statistics->playback_buffer_write_age_us =
			playback_timing_statistics.playback_buffer_write_age_us;
	statistics->playback_worker_visit_age_us =
			playback_timing_statistics.worker_visit_age_us;
	statistics->playback_maximum_gap = playback_timing_statistics.maximum_gap;
	statistics->playback_last_underflow =
			playback_timing_statistics.last_underflow;
	statistics->playback_deferred_callbacks = atomic_load_explicit(
			&stream_context->playback_deferred_callback_count,
			memory_order_relaxed);
}

static switch_status_t write_json_document(
		switch_stream_handle_t *stream,
		cJSON *document)
{
	char *serialized = cJSON_PrintUnformatted(document);

	if (!serialized) {
		return SWITCH_STATUS_MEMERR;
	}

	stream->write_function(stream, "%s\n", serialized);
	cJSON_free(serialized);
	return SWITCH_STATUS_SUCCESS;
}

static void write_json_error(
		switch_stream_handle_t *stream,
		const char *error,
		const char *detail_name,
		const char *detail_value)
{
	cJSON *document = cJSON_CreateObject();

	if (!document ||
			!cJSON_AddBoolToObject(document, "ok", 0) ||
			!cJSON_AddStringToObject(document, "error", error) ||
			(!zstr(detail_name) &&
			 !cJSON_AddStringToObject(
				document,
				detail_name,
				detail_value ? detail_value : "")) ||
			write_json_document(stream, document) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR %s\n", error);
	}

	if (document) {
		cJSON_Delete(document);
	}
}

static switch_status_t add_stream_status_json(
		cJSON *streams,
		mws_stream_t *stream_context)
{
	mws_stream_statistics_t statistics;
	const char *channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	const char *websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);
	cJSON *stream_status = cJSON_CreateObject();

	if (!stream_status) {
		return SWITCH_STATUS_MEMERR;
	}

	snapshot_stream_statistics(stream_context, &statistics);
	if (!cJSON_AddStringToObject(
			stream_status,
			"stream_id",
			stream_context->stream_id) ||
			!cJSON_AddStringToObject(
				stream_status,
				"state",
				transport_state_name(statistics.transport_state)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"target_rate_hz",
				stream_context->target_sample_rate) ||
			!cJSON_AddStringToObject(
				stream_status,
				"channel_to_websocket",
				channel_to_websocket ? channel_to_websocket : "unknown") ||
			!cJSON_AddStringToObject(
				stream_status,
				"websocket_to_channel",
				websocket_to_channel ? websocket_to_channel : "unknown") ||
			!cJSON_AddBoolToObject(
				stream_status,
				"api_commands",
				stream_context->api_commands_enabled ? 1 : 0) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"message_events",
				(double) atomic_load_explicit(
					&stream_context->message_event_count,
					memory_order_relaxed)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"rejected_messages",
				(double) atomic_load_explicit(
					&stream_context->rejected_message_count,
					memory_order_relaxed)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"outbound_messages_sent",
				(double) atomic_load_explicit(
					&stream_context->outbound_message_sent_count,
					memory_order_relaxed)) ||
			!cJSON_AddStringToObject(
				stream_status,
				"playback_state",
				playback_state_name(statistics.playback_state)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"captured_queued_bytes",
				(double) statistics.captured_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"captured_capacity_bytes",
				(double) statistics.captured_capacity_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"fallback_queued_bytes",
				(double) statistics.fallback_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_source_queued_bytes",
				(double) statistics.playback_source_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_source_capacity_bytes",
				(double) statistics.playback_source_capacity_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_queued_bytes",
				(double) statistics.playback_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_capacity_bytes",
				(double) statistics.playback_capacity_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_peak_queued_bytes",
				(double) statistics.playback_peak_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"captured_drop_events",
				(double) statistics.captured_drop_events) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"captured_dropped_bytes",
				(double) statistics.captured_dropped_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_received_samples",
				(double) statistics.playback_received_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_source_dropped_samples",
				(double) statistics.playback_source_dropped_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_buffered_samples",
				(double) statistics.playback_buffered_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_mixed_samples",
				(double) statistics.playback_mixed_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_dropped_samples",
				(double) statistics.playback_dropped_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_frames",
				(double) statistics.playback_underflow_frames) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_invalid_frames",
				(double) statistics.playback_invalid_frames) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callbacks",
				(double) statistics.playback_callbacks) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_requested_samples",
				(double) statistics.playback_requested_samples) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callback_intervals",
				(double) statistics.playback_callback_intervals) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callback_interval_total_us",
				(double) statistics.playback_callback_interval_total_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callback_max_interval_us",
				(double) statistics.playback_callback_max_interval_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callback_max_lateness_us",
				(double) statistics.playback_callback_max_lateness_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_callback_gap_intervals",
				(double) statistics.playback_callback_gap_intervals) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_module_dispatch_max_delay_us",
				(double) statistics.playback_module_dispatch_max_delay_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_with_source_audio_count",
				(double) statistics.playback_underflow_with_source_audio_count) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_observation_snapshot_misses",
				(double) statistics.playback_observation_snapshot_misses) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_websocket_audio_seen",
				statistics.playback_websocket_audio_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_buffer_write_seen",
				statistics.playback_buffer_write_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_worker_visit_seen",
				statistics.playback_worker_visit_seen) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_websocket_audio_age_us",
				(double) statistics.playback_websocket_audio_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_buffer_write_age_us",
				(double) statistics.playback_buffer_write_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_worker_visit_age_us",
				(double) statistics.playback_worker_visit_age_us) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_max_gap_observation_valid",
				statistics.playback_maximum_gap.valid) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_observed_at_monotonic_us",
				(double) statistics.playback_maximum_gap.
					observed_at_monotonic_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_age_us",
				(double) statistics.playback_maximum_gap.observation_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_callback_interval_us",
				(double) statistics.playback_maximum_gap.
					callback_entry_interval_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_expected_interval_us",
				(double) statistics.playback_maximum_gap.
					callback_expected_interval_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_lateness_us",
				(double) statistics.playback_maximum_gap.callback_lateness_us) ||
			!cJSON_AddStringToObject(
				stream_status,
				"playback_max_gap_state",
				playback_state_name((mws_playback_state_t)
					statistics.playback_maximum_gap.playback_state)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_dispatch_delay_us",
				(double) statistics.playback_maximum_gap.module_dispatch_delay_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_source_queued_bytes",
				(double) statistics.playback_maximum_gap.source_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_queued_bytes",
				(double) statistics.playback_maximum_gap.playback_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_websocket_audio_age_us",
				(double) statistics.playback_maximum_gap.websocket_audio_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_buffer_write_age_us",
				(double) statistics.playback_maximum_gap.playback_buffer_write_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_max_gap_worker_visit_age_us",
				(double) statistics.playback_maximum_gap.worker_visit_age_us) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_max_gap_websocket_audio_seen",
				statistics.playback_maximum_gap.websocket_audio_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_max_gap_buffer_write_seen",
				statistics.playback_maximum_gap.playback_buffer_write_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_max_gap_worker_visit_seen",
				statistics.playback_maximum_gap.worker_visit_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_underflow_observation_valid",
				statistics.playback_last_underflow.valid) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_observed_at_monotonic_us",
				(double) statistics.playback_last_underflow.
					observed_at_monotonic_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_age_us",
				(double) statistics.playback_last_underflow.observation_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_callback_interval_us",
				(double) statistics.playback_last_underflow.
					callback_entry_interval_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_expected_interval_us",
				(double) statistics.playback_last_underflow.
					callback_expected_interval_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_callback_lateness_us",
				(double) statistics.playback_last_underflow.callback_lateness_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_dispatch_delay_us",
				(double) statistics.playback_last_underflow.module_dispatch_delay_us) ||
			!cJSON_AddStringToObject(
				stream_status,
				"playback_underflow_state",
				playback_state_name((mws_playback_state_t)
					statistics.playback_last_underflow.playback_state)) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_source_queued_bytes",
				(double) statistics.playback_last_underflow.source_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_queued_bytes",
				(double) statistics.playback_last_underflow.playback_queued_bytes) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_websocket_audio_age_us",
				(double) statistics.playback_last_underflow.websocket_audio_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_buffer_write_age_us",
				(double) statistics.playback_last_underflow.playback_buffer_write_age_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_underflow_worker_visit_age_us",
				(double) statistics.playback_last_underflow.worker_visit_age_us) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_underflow_websocket_audio_seen",
				statistics.playback_last_underflow.websocket_audio_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_underflow_buffer_write_seen",
				statistics.playback_last_underflow.playback_buffer_write_seen) ||
			!cJSON_AddBoolToObject(
				stream_status,
				"playback_underflow_worker_visit_seen",
				statistics.playback_last_underflow.worker_visit_seen) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_deferred_callbacks",
				(double) statistics.playback_deferred_callbacks) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_queued_duration_us",
				(double) statistics.playback_queued_duration_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_target_queue_us",
				(double) statistics.playback_rate_target_queue_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_filtered_queue_us",
				(double) statistics.playback_rate_filtered_queue_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_adjustment_ppm",
				statistics.playback_rate_adjustment_ppm) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_maximum_adjustment_ppm",
				statistics.playback_rate_maximum_adjustment_ppm) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_updates",
				(double) statistics.playback_rate_updates) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"playback_rate_limit_duration_us",
				(double) statistics.playback_rate_limit_duration_us) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_attempt_limit",
				stream_context->reconnect_attempt_limit) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_delay_ms",
				stream_context->reconnect_delay_ms) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_attempt",
				statistics.consecutive_reconnect_attempts) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_attempts_total",
				(double) statistics.reconnect_attempt_count) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_successes",
				(double) statistics.reconnect_success_count) ||
			!cJSON_AddNumberToObject(
				stream_status,
				"reconnect_in_ms",
				(double) statistics.reconnect_in_ms)) {
		cJSON_Delete(stream_status);
		return SWITCH_STATUS_MEMERR;
	}

	cJSON_AddItemToArray(streams, stream_status);
	return SWITCH_STATUS_SUCCESS;
}

static void write_stream_status(
		switch_stream_handle_t *stream,
		mws_stream_t *stream_context)
{
	mws_stream_statistics_t statistics;
	const char *channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	const char *websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);

	snapshot_stream_statistics(stream_context, &statistics);
	stream->write_function(
			stream,
			"stream_id=%s state=%s target_rate_hz=%d "
			"channel_to_websocket=%s websocket_to_channel=%s "
			"api_commands=%s "
			"message_events=%" SWITCH_UINT64_T_FMT " "
			"rejected_messages=%" SWITCH_UINT64_T_FMT " "
			"outbound_messages_sent=%" SWITCH_UINT64_T_FMT " "
			"playback_state=%s "
			"captured_queued_bytes=%" SWITCH_SIZE_T_FMT " "
			"captured_capacity_bytes=%" SWITCH_SIZE_T_FMT " "
			"fallback_queued_bytes=%" SWITCH_SIZE_T_FMT " "
			"playback_source_queued_bytes=%" SWITCH_SIZE_T_FMT " "
			"playback_source_capacity_bytes=%" SWITCH_SIZE_T_FMT " "
			"playback_queued_bytes=%" SWITCH_SIZE_T_FMT " "
			"playback_capacity_bytes=%" SWITCH_SIZE_T_FMT " "
			"playback_peak_queued_bytes=%" SWITCH_SIZE_T_FMT " "
			"captured_drop_events=%" SWITCH_UINT64_T_FMT " "
			"captured_dropped_bytes=%" SWITCH_UINT64_T_FMT " "
			"playback_received_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_source_dropped_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_buffered_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_mixed_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_dropped_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_frames=%" SWITCH_UINT64_T_FMT " "
			"playback_invalid_frames=%" SWITCH_UINT64_T_FMT " "
			"playback_callbacks=%" SWITCH_UINT64_T_FMT " "
			"playback_requested_samples=%" SWITCH_UINT64_T_FMT " "
			"playback_callback_intervals=%" SWITCH_UINT64_T_FMT " "
			"playback_callback_interval_total_us=%" SWITCH_UINT64_T_FMT " "
			"playback_callback_max_interval_us=%" SWITCH_UINT64_T_FMT " "
			"playback_callback_max_lateness_us=%" SWITCH_UINT64_T_FMT " "
			"playback_callback_gap_intervals=%" SWITCH_UINT64_T_FMT " "
			"playback_module_dispatch_max_delay_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_with_source_audio_count=%" SWITCH_UINT64_T_FMT " "
			"playback_observation_snapshot_misses=%" SWITCH_UINT64_T_FMT " "
			"playback_websocket_audio_seen=%d "
			"playback_buffer_write_seen=%d "
			"playback_worker_visit_seen=%d "
			"playback_websocket_audio_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_buffer_write_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_worker_visit_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_observation_valid=%d "
			"playback_max_gap_observed_at_monotonic_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_callback_interval_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_expected_interval_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_lateness_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_state=%s "
			"playback_max_gap_dispatch_delay_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_source_queued_bytes=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_queued_bytes=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_websocket_audio_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_buffer_write_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_worker_visit_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_max_gap_websocket_audio_seen=%d "
			"playback_max_gap_buffer_write_seen=%d "
			"playback_max_gap_worker_visit_seen=%d "
			"playback_underflow_observation_valid=%d "
			"playback_underflow_observed_at_monotonic_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_callback_interval_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_expected_interval_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_callback_lateness_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_dispatch_delay_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_state=%s "
			"playback_underflow_source_queued_bytes=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_queued_bytes=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_websocket_audio_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_buffer_write_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_worker_visit_age_us=%" SWITCH_UINT64_T_FMT " "
			"playback_underflow_websocket_audio_seen=%d "
			"playback_underflow_buffer_write_seen=%d "
			"playback_underflow_worker_visit_seen=%d "
			"playback_deferred_callbacks=%" SWITCH_UINT64_T_FMT " "
			"playback_queued_duration_us=%" SWITCH_UINT64_T_FMT " "
			"playback_rate_target_queue_us=%" SWITCH_UINT64_T_FMT " "
			"playback_rate_filtered_queue_us=%" SWITCH_UINT64_T_FMT " "
			"playback_rate_adjustment_ppm=%d "
			"playback_rate_maximum_adjustment_ppm=%u "
			"playback_rate_updates=%" SWITCH_UINT64_T_FMT " "
			"playback_rate_limit_duration_us=%" SWITCH_UINT64_T_FMT " "
			"reconnect_attempt_limit=%d reconnect_delay_ms=%d "
			"reconnect_attempt=%u "
			"reconnect_attempts_total=%" SWITCH_UINT64_T_FMT " "
			"reconnect_successes=%" SWITCH_UINT64_T_FMT " "
			"reconnect_in_ms=%" SWITCH_UINT64_T_FMT "\n",
			stream_context->stream_id,
			transport_state_name(statistics.transport_state),
			stream_context->target_sample_rate,
			channel_to_websocket ? channel_to_websocket : "unknown",
			websocket_to_channel ? websocket_to_channel : "unknown",
			stream_context->api_commands_enabled ? "true" : "false",
			atomic_load_explicit(
					&stream_context->message_event_count,
					memory_order_relaxed),
			atomic_load_explicit(
					&stream_context->rejected_message_count,
					memory_order_relaxed),
			atomic_load_explicit(
					&stream_context->outbound_message_sent_count,
					memory_order_relaxed),
			playback_state_name(statistics.playback_state),
			statistics.captured_queued_bytes,
			statistics.captured_capacity_bytes,
			statistics.fallback_queued_bytes,
			statistics.playback_source_queued_bytes,
			statistics.playback_source_capacity_bytes,
			statistics.playback_queued_bytes,
			statistics.playback_capacity_bytes,
			statistics.playback_peak_queued_bytes,
			statistics.captured_drop_events,
			statistics.captured_dropped_bytes,
			statistics.playback_received_samples,
			statistics.playback_source_dropped_samples,
			statistics.playback_buffered_samples,
			statistics.playback_mixed_samples,
			statistics.playback_dropped_samples,
			statistics.playback_underflow_frames,
			statistics.playback_invalid_frames,
			statistics.playback_callbacks,
			statistics.playback_requested_samples,
			statistics.playback_callback_intervals,
			statistics.playback_callback_interval_total_us,
			statistics.playback_callback_max_interval_us,
			statistics.playback_callback_max_lateness_us,
			statistics.playback_callback_gap_intervals,
			statistics.playback_module_dispatch_max_delay_us,
			statistics.playback_underflow_with_source_audio_count,
			statistics.playback_observation_snapshot_misses,
			statistics.playback_websocket_audio_seen ? 1 : 0,
			statistics.playback_buffer_write_seen ? 1 : 0,
			statistics.playback_worker_visit_seen ? 1 : 0,
			statistics.playback_websocket_audio_age_us,
			statistics.playback_buffer_write_age_us,
			statistics.playback_worker_visit_age_us,
			statistics.playback_maximum_gap.valid ? 1 : 0,
			statistics.playback_maximum_gap.observed_at_monotonic_us,
			statistics.playback_maximum_gap.observation_age_us,
			statistics.playback_maximum_gap.callback_entry_interval_us,
			statistics.playback_maximum_gap.callback_expected_interval_us,
			statistics.playback_maximum_gap.callback_lateness_us,
			playback_state_name((mws_playback_state_t)
				statistics.playback_maximum_gap.playback_state),
			statistics.playback_maximum_gap.module_dispatch_delay_us,
			statistics.playback_maximum_gap.source_queued_bytes,
			statistics.playback_maximum_gap.playback_queued_bytes,
			statistics.playback_maximum_gap.websocket_audio_age_us,
			statistics.playback_maximum_gap.playback_buffer_write_age_us,
			statistics.playback_maximum_gap.worker_visit_age_us,
			statistics.playback_maximum_gap.websocket_audio_seen ? 1 : 0,
			statistics.playback_maximum_gap.playback_buffer_write_seen ? 1 : 0,
			statistics.playback_maximum_gap.worker_visit_seen ? 1 : 0,
			statistics.playback_last_underflow.valid ? 1 : 0,
			statistics.playback_last_underflow.observed_at_monotonic_us,
			statistics.playback_last_underflow.observation_age_us,
			statistics.playback_last_underflow.callback_entry_interval_us,
			statistics.playback_last_underflow.callback_expected_interval_us,
			statistics.playback_last_underflow.callback_lateness_us,
			statistics.playback_last_underflow.module_dispatch_delay_us,
			playback_state_name((mws_playback_state_t)
				statistics.playback_last_underflow.playback_state),
			statistics.playback_last_underflow.source_queued_bytes,
			statistics.playback_last_underflow.playback_queued_bytes,
			statistics.playback_last_underflow.websocket_audio_age_us,
			statistics.playback_last_underflow.playback_buffer_write_age_us,
			statistics.playback_last_underflow.worker_visit_age_us,
			statistics.playback_last_underflow.websocket_audio_seen ? 1 : 0,
			statistics.playback_last_underflow.playback_buffer_write_seen ? 1 : 0,
			statistics.playback_last_underflow.worker_visit_seen ? 1 : 0,
			statistics.playback_deferred_callbacks,
			statistics.playback_queued_duration_us,
			statistics.playback_rate_target_queue_us,
			statistics.playback_rate_filtered_queue_us,
			statistics.playback_rate_adjustment_ppm,
			statistics.playback_rate_maximum_adjustment_ppm,
			statistics.playback_rate_updates,
			statistics.playback_rate_limit_duration_us,
			stream_context->reconnect_attempt_limit,
			stream_context->reconnect_delay_ms,
			statistics.consecutive_reconnect_attempts,
			statistics.reconnect_attempt_count,
			statistics.reconnect_success_count,
			statistics.reconnect_in_ms);
}

static void unregister_stream(mws_stream_t *stream_context)
{
	mws_stream_t **current;
	switch_memory_pool_t *pool = NULL;

	switch_mutex_lock(globals.streams_mutex);

	for (current = &globals.streams; *current; current = &(*current)->next) {
		if (*current == stream_context) {
			*current = stream_context->next;
			stream_context->next = NULL;
			if (globals.active_streams > 0) {
				globals.active_streams--;
			}
			if (stream_context->admission_reserved) {
				if (stream_context->admission_channel_context) {
					switch_assert(
							stream_context->admission_channel_context->
							reserved_streams > 0);
					stream_context->admission_channel_context->
							reserved_streams--;
					stream_context->admission_channel_context = NULL;
				}
				if (!stream_context->cleanup_holds_stream_slot) {
					switch_assert(globals.reserved_streams > 0);
					globals.reserved_streams--;
					stream_context->admission_reserved = SWITCH_FALSE;
				}
			}
			stream_context->resource_release_requested = SWITCH_TRUE;
			pool = claim_stream_resource_pool_locked(stream_context);
			break;
		}
	}

	switch_mutex_unlock(globals.streams_mutex);
	destroy_stream_resource_pool(stream_context, pool);
}

static void finish_detached_stream_cleanup(mws_stream_t *stream_context)
{
	mws_captured_audio_resampler_statistics_t
			captured_resampler_statistics;
	mws_playback_audio_resampler_statistics_t
			playback_resampler_statistics;
	mws_stream_statistics_t final_statistics;
	switch_size_t native_ring_buffered_bytes = 0;
	switch_status_t worker_status;

	stream_context->channel_next = NULL;
	wait_for_stream_startup(stream_context);
	worker_status = stop_stream_worker(stream_context);
	if (worker_status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
			SWITCH_CHANNEL_UUID_LOG(stream_context->session_uuid),
			SWITCH_LOG_CRIT,
			"Could not detach stream from transport worker\n");
	}

	snapshot_stream_statistics(
			stream_context,
			&final_statistics);
	mws_captured_audio_resampler_snapshot(
			&stream_context->captured_audio_resampler,
			&captured_resampler_statistics);
	mws_playback_audio_resampler_snapshot(
			&stream_context->playback_audio_resampler,
			&playback_resampler_statistics);

	native_ring_buffered_bytes =
			mws_captured_audio_buffer_discard_ring(
				&stream_context->captured_audio_buffer);

	switch_log_printf(
		SWITCH_CHANNEL_UUID_LOG(stream_context->session_uuid),
		SWITCH_LOG_NOTICE,
		"WebSocket stream cleanup complete: stream_id=%s "
		"captured_media_frames=%" SWITCH_UINT64_T_FMT
		" captured_native_bytes=%" SWITCH_UINT64_T_FMT
		" libcurl_submitted_frames=%" SWITCH_UINT64_T_FMT
		" libcurl_submitted_bytes=%" SWITCH_UINT64_T_FMT
		" text_frames_sent=%" SWITCH_UINT64_T_FMT
		" text_bytes_sent=%" SWITCH_UINT64_T_FMT
		" native_pcm_consumed_bytes=%" SWITCH_UINT64_T_FMT
		" target_pcm_produced_bytes=%" SWITCH_UINT64_T_FMT
		" native_ring_buffered_bytes=%" SWITCH_SIZE_T_FMT
		" native_pcm_processing_failed_bytes=%" SWITCH_UINT64_T_FMT
		" pending_target_pcm_dropped_bytes=%" SWITCH_UINT64_T_FMT
		" native_drop_events=%" SWITCH_UINT64_T_FMT
		" native_dropped_bytes=%" SWITCH_UINT64_T_FMT
		" native_format_generation=%" SWITCH_UINT64_T_FMT
		" invalid_native_frames=%" SWITCH_UINT64_T_FMT
		" media_callback_budget_hits=%" SWITCH_UINT64_T_FMT
		" native_ring_lock_contentions=%" SWITCH_UINT64_T_FMT
		" fallback_chunk_drops=%" SWITCH_UINT64_T_FMT
		" fallback_chunk_stores=%" SWITCH_UINT64_T_FMT
		" fallback_chunk_overwrites=%" SWITCH_UINT64_T_FMT
		" fallback_chunk_overwritten_bytes=%" SWITCH_UINT64_T_FMT
		" fallback_chunk_abandoned_bytes=%" SWITCH_UINT64_T_FMT
		" stale_ring_discarded_bytes=%" SWITCH_UINT64_T_FMT
		" worker_source_format_changes=%" SWITCH_UINT64_T_FMT
		" resampler_creates=%" SWITCH_UINT64_T_FMT
		" resampler_errors=%" SWITCH_UINT64_T_FMT
		" native_rate_hz=%u target_rate_hz=%d native_channels=%u"
		" transport=%s curl_result=%d multi_result=%d"
		" inbound_frames=%" SWITCH_UINT64_T_FMT
		" inbound_bytes=%" SWITCH_UINT64_T_FMT
		" inbound_binary_frames=%" SWITCH_UINT64_T_FMT
		" inbound_binary_bytes=%" SWITCH_UINT64_T_FMT
		" inbound_text_frames=%" SWITCH_UINT64_T_FMT
		" inbound_text_bytes=%" SWITCH_UINT64_T_FMT
		" clear_commands=%" SWITCH_UINT64_T_FMT
		" clear_discarded_samples=%" SWITCH_UINT64_T_FMT
		" api_commands_received=%" SWITCH_UINT64_T_FMT
		" api_commands_executed=%" SWITCH_UINT64_T_FMT
		" api_commands_failed=%" SWITCH_UINT64_T_FMT
		" api_command_result_bytes=%" SWITCH_UINT64_T_FMT
		" rejected_api_commands=%" SWITCH_UINT64_T_FMT
		" dropped_text_responses=%" SWITCH_UINT64_T_FMT
		" invalid_inbound_chunks=%" SWITCH_UINT64_T_FMT
		" playback_received_samples=%" SWITCH_UINT64_T_FMT
		" playback_source_dropped_samples=%" SWITCH_UINT64_T_FMT
		" playback_buffered_samples=%" SWITCH_UINT64_T_FMT
		" playback_mixed_samples=%" SWITCH_UINT64_T_FMT
		" playback_dropped_samples=%" SWITCH_UINT64_T_FMT
		" playback_underflow_frames=%" SWITCH_UINT64_T_FMT
		" playback_invalid_frames=%" SWITCH_UINT64_T_FMT
		" playback_callbacks=%" SWITCH_UINT64_T_FMT
		" playback_requested_samples=%" SWITCH_UINT64_T_FMT
		" playback_callback_intervals=%" SWITCH_UINT64_T_FMT
		" playback_callback_interval_total_us=%" SWITCH_UINT64_T_FMT
		" playback_callback_max_interval_us=%" SWITCH_UINT64_T_FMT
		" playback_deferred_callbacks=%" SWITCH_UINT64_T_FMT
		" playback_queued_duration_us=%" SWITCH_UINT64_T_FMT
		" playback_rate_target_queue_us=%" SWITCH_UINT64_T_FMT
		" playback_rate_filtered_queue_us=%" SWITCH_UINT64_T_FMT
		" playback_rate_adjustment_ppm=%d"
		" playback_rate_maximum_adjustment_ppm=%u"
		" playback_rate_updates=%" SWITCH_UINT64_T_FMT
		" playback_rate_limit_duration_us=%" SWITCH_UINT64_T_FMT
		" playback_format_changes=%" SWITCH_UINT64_T_FMT
		" playback_resampler_creates=%" SWITCH_UINT64_T_FMT
		" playback_resampler_errors=%" SWITCH_UINT64_T_FMT "\n",
		stream_context->stream_id,
		stream_context->captured_media_frame_count,
		stream_context->captured_native_byte_count,
		stream_context->libcurl_submitted_frame_count,
		stream_context->libcurl_submitted_byte_count,
		stream_context->text_frame_sent_count,
		stream_context->text_byte_sent_count,
		captured_resampler_statistics.source_consumed_bytes,
		captured_resampler_statistics.target_produced_bytes,
		native_ring_buffered_bytes,
		captured_resampler_statistics.
			source_processing_failed_bytes,
		captured_resampler_statistics.
			pending_target_dropped_bytes,
		final_statistics.captured_drop_events,
		final_statistics.captured_dropped_bytes,
		final_statistics.captured_format_generation,
		final_statistics.captured_invalid_frames,
		stream_context->media_callback_budget_hit_count,
		final_statistics.captured_ring_lock_contentions,
		final_statistics.captured_fallback_drops,
		final_statistics.captured_fallback_stores,
		final_statistics.captured_fallback_overwrites,
		final_statistics.captured_fallback_overwritten_bytes,
		final_statistics.captured_fallback_abandoned_bytes,
		final_statistics.captured_stale_ring_discarded_bytes,
		captured_resampler_statistics.source_format_changes,
		captured_resampler_statistics.resampler_creates,
		captured_resampler_statistics.resampler_errors,
		captured_resampler_statistics.source_sample_rate,
		stream_context->target_sample_rate,
		captured_resampler_statistics.source_channels,
		transport_state_name(
				stream_context->transport_terminal_state),
		(int) stream_context->curl_result,
		(int) stream_context->multi_result,
		stream_context->inbound_frame_count,
		stream_context->inbound_byte_count,
		stream_context->inbound_binary_frame_count,
		stream_context->inbound_binary_byte_count,
		stream_context->inbound_text_frame_count,
		stream_context->inbound_text_byte_count,
		stream_context->clear_command_count,
		stream_context->clear_discarded_sample_count,
		stream_context->api_command_received_count,
		stream_context->api_command_executed_count,
		stream_context->api_command_failed_count,
		stream_context->api_command_result_byte_count,
		atomic_load_explicit(
				&stream_context->rejected_api_command_count,
				memory_order_relaxed),
		stream_context->dropped_text_response_count,
		stream_context->invalid_inbound_chunk_count,
		final_statistics.playback_received_samples,
		final_statistics.playback_source_dropped_samples,
		final_statistics.playback_buffered_samples,
		final_statistics.playback_mixed_samples,
		final_statistics.playback_dropped_samples,
		final_statistics.playback_underflow_frames,
		final_statistics.playback_invalid_frames,
		final_statistics.playback_callbacks,
		final_statistics.playback_requested_samples,
		final_statistics.playback_callback_intervals,
		final_statistics.playback_callback_interval_total_us,
		final_statistics.playback_callback_max_interval_us,
		final_statistics.playback_deferred_callbacks,
		final_statistics.playback_queued_duration_us,
		final_statistics.playback_rate_target_queue_us,
		final_statistics.playback_rate_filtered_queue_us,
		final_statistics.playback_rate_adjustment_ppm,
		final_statistics.playback_rate_maximum_adjustment_ppm,
		final_statistics.playback_rate_updates,
		final_statistics.playback_rate_limit_duration_us,
		atomic_load_explicit(
				&stream_context->playback_format_change_count,
				memory_order_relaxed),
		playback_resampler_statistics.resampler_create_count,
		playback_resampler_statistics.resampler_error_count);
	switch_log_printf(
		SWITCH_CHANNEL_UUID_LOG(stream_context->session_uuid),
		SWITCH_LOG_NOTICE,
		"WebSocket playback callback timing: stream_id=%s"
		" callback_max_interval_us=%" SWITCH_UINT64_T_FMT
		" callback_max_lateness_us=%" SWITCH_UINT64_T_FMT
		" callback_gap_intervals=%" SWITCH_UINT64_T_FMT
		" module_dispatch_max_delay_us=%" SWITCH_UINT64_T_FMT
		" observation_snapshot_misses=%" SWITCH_UINT64_T_FMT
		" max_gap_valid=%d"
		" max_gap_observed_at_monotonic_us=%" SWITCH_UINT64_T_FMT
		" max_gap_age_us=%" SWITCH_UINT64_T_FMT
		" max_gap_callback_interval_us=%" SWITCH_UINT64_T_FMT
		" max_gap_expected_interval_us=%" SWITCH_UINT64_T_FMT
		" max_gap_lateness_us=%" SWITCH_UINT64_T_FMT
		" max_gap_state=%s"
		" max_gap_dispatch_delay_us=%" SWITCH_UINT64_T_FMT
		" max_gap_source_queued_bytes=%" SWITCH_UINT64_T_FMT
		" max_gap_queued_bytes=%" SWITCH_UINT64_T_FMT
		" max_gap_websocket_audio_seen=%d"
		" max_gap_websocket_audio_age_us=%" SWITCH_UINT64_T_FMT
		" max_gap_buffer_write_seen=%d"
		" max_gap_buffer_write_age_us=%" SWITCH_UINT64_T_FMT
		" max_gap_worker_visit_seen=%d"
		" max_gap_worker_visit_age_us=%" SWITCH_UINT64_T_FMT "\n",
		stream_context->stream_id,
		final_statistics.playback_callback_max_interval_us,
		final_statistics.playback_callback_max_lateness_us,
		final_statistics.playback_callback_gap_intervals,
		final_statistics.playback_module_dispatch_max_delay_us,
		final_statistics.playback_observation_snapshot_misses,
		final_statistics.playback_maximum_gap.valid ? 1 : 0,
		final_statistics.playback_maximum_gap.
			observed_at_monotonic_us,
		final_statistics.playback_maximum_gap.observation_age_us,
		final_statistics.playback_maximum_gap.
			callback_entry_interval_us,
		final_statistics.playback_maximum_gap.
			callback_expected_interval_us,
		final_statistics.playback_maximum_gap.callback_lateness_us,
		playback_state_name((mws_playback_state_t)
			final_statistics.playback_maximum_gap.playback_state),
		final_statistics.playback_maximum_gap.
			module_dispatch_delay_us,
		final_statistics.playback_maximum_gap.source_queued_bytes,
		final_statistics.playback_maximum_gap.playback_queued_bytes,
		final_statistics.playback_maximum_gap.websocket_audio_seen ?
			1 : 0,
		final_statistics.playback_maximum_gap.websocket_audio_age_us,
		final_statistics.playback_maximum_gap.
			playback_buffer_write_seen ? 1 : 0,
		final_statistics.playback_maximum_gap.
			playback_buffer_write_age_us,
		final_statistics.playback_maximum_gap.worker_visit_seen ? 1 : 0,
		final_statistics.playback_maximum_gap.worker_visit_age_us);
	switch_log_printf(
		SWITCH_CHANNEL_UUID_LOG(stream_context->session_uuid),
		SWITCH_LOG_NOTICE,
		"WebSocket playback underflow timing: stream_id=%s"
		" underflow_count=%" SWITCH_UINT64_T_FMT
		" underflow_with_source_audio_count=%" SWITCH_UINT64_T_FMT
		" underflow_valid=%d"
		" underflow_observed_at_monotonic_us=%" SWITCH_UINT64_T_FMT
		" underflow_age_us=%" SWITCH_UINT64_T_FMT
		" underflow_callback_interval_us=%" SWITCH_UINT64_T_FMT
		" underflow_expected_interval_us=%" SWITCH_UINT64_T_FMT
		" underflow_callback_lateness_us=%" SWITCH_UINT64_T_FMT
		" underflow_state=%s"
		" underflow_dispatch_delay_us=%" SWITCH_UINT64_T_FMT
		" underflow_source_queued_bytes=%" SWITCH_UINT64_T_FMT
		" underflow_queued_bytes=%" SWITCH_UINT64_T_FMT
		" underflow_websocket_audio_seen=%d"
		" underflow_websocket_audio_age_us=%" SWITCH_UINT64_T_FMT
		" underflow_buffer_write_seen=%d"
		" underflow_buffer_write_age_us=%" SWITCH_UINT64_T_FMT
		" underflow_worker_visit_seen=%d"
		" underflow_worker_visit_age_us=%" SWITCH_UINT64_T_FMT "\n",
		stream_context->stream_id,
		final_statistics.playback_underflow_frames,
		final_statistics.playback_underflow_with_source_audio_count,
		final_statistics.playback_last_underflow.valid ? 1 : 0,
		final_statistics.playback_last_underflow.
			observed_at_monotonic_us,
		final_statistics.playback_last_underflow.observation_age_us,
		final_statistics.playback_last_underflow.
			callback_entry_interval_us,
		final_statistics.playback_last_underflow.
			callback_expected_interval_us,
		final_statistics.playback_last_underflow.callback_lateness_us,
		playback_state_name((mws_playback_state_t)
			final_statistics.playback_last_underflow.playback_state),
		final_statistics.playback_last_underflow.
			module_dispatch_delay_us,
		final_statistics.playback_last_underflow.source_queued_bytes,
		final_statistics.playback_last_underflow.playback_queued_bytes,
		final_statistics.playback_last_underflow.websocket_audio_seen ?
			1 : 0,
		final_statistics.playback_last_underflow.
			websocket_audio_age_us,
		final_statistics.playback_last_underflow.
			playback_buffer_write_seen ? 1 : 0,
		final_statistics.playback_last_underflow.
			playback_buffer_write_age_us,
		final_statistics.playback_last_underflow.worker_visit_seen ?
			1 : 0,
		final_statistics.playback_last_underflow.worker_visit_age_us);
	if (stream_is_registered(stream_context)) {
		unregister_stream(stream_context);
	}
	release_stream_resource_borrow(stream_context);
}
static switch_status_t load_config(switch_bool_t reload)
{
	switch_xml_t xml = NULL;
	switch_xml_t configuration = NULL;
	switch_xml_t settings;
	mws_http_header_t *new_headers = NULL;
	mws_http_header_t *old_headers = NULL;
	mws_config_snapshot_t old_config;
	switch_status_t status;

	if (!(xml = switch_xml_open_cfg(
			"websocket_stream.conf",
			&configuration,
			NULL))) {
		return SWITCH_STATUS_FALSE;
	}

	if (validate_configured_limits(configuration) != SWITCH_STATUS_SUCCESS) {
		switch_xml_free(xml);
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(globals.config_mutex);
	snapshot_config_locked(&old_config);

	settings = switch_xml_child(configuration, "settings");
	status = settings ? switch_xml_config_parse(
			switch_xml_child(settings, "param"),
			reload,
			config_instructions) : SWITCH_STATUS_SUCCESS;
	if (status == SWITCH_STATUS_SUCCESS) {
		status = parse_configured_headers(
				configuration,
				&new_headers,
				(uint32_t) globals.maximum_http_headers,
				(switch_size_t) globals.maximum_http_header_bytes);
	}
	if (status == SWITCH_STATUS_SUCCESS) {
		old_headers = globals.configured_headers;
		globals.configured_headers = new_headers;
		globals.configured_header_count = mws_http_headers_count(new_headers);
		new_headers = NULL;
	} else {
		restore_config_locked(&old_config);
	}

	switch_mutex_unlock(globals.config_mutex);
	mws_http_headers_free(old_headers);
	mws_http_headers_free(new_headers);
	switch_xml_free(xml);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"Could not load websocket_stream.conf.xml\n");

		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static void cleanup_config(void)
{
	mws_http_header_t *configured_headers;

	switch_mutex_lock(globals.config_mutex);
	configured_headers = globals.configured_headers;
	globals.configured_headers = NULL;
	globals.configured_header_count = 0;
	switch_xml_config_cleanup(config_instructions);
	switch_mutex_unlock(globals.config_mutex);

	mws_http_headers_free(configured_headers);
}

static void fan_out_captured_audio(
		mws_channel_t *channel_context,
		switch_frame_t *frame)
{
	int16_t mono_pcm[SWITCH_RECOMMENDED_BUFFER_SIZE / sizeof(int16_t)];
	const int16_t *stereo_pcm;
	switch_size_t sample_count;
	switch_size_t sample_index;
	mws_stream_t *stream_context;
	mws_channel_to_websocket_t channel_to_websocket;

	if (!frame || !frame->data || !frame->datalen || !frame->rate ||
			frame->channels != 2 ||
			frame->datalen > SWITCH_RECOMMENDED_BUFFER_SIZE ||
			frame->datalen % (2 * sizeof(int16_t))) {
		switch_mutex_lock(channel_context->mutex);
		for (stream_context = channel_context->streams; stream_context;
				stream_context = stream_context->channel_next) {
			mws_captured_audio_buffer_record_invalid_frame(
					&stream_context->captured_audio_buffer);
		}
		switch_mutex_unlock(channel_context->mutex);
		return;
	}

	stereo_pcm = (const int16_t *) frame->data;
	sample_count = frame->datalen / (2 * sizeof(int16_t));

	switch_mutex_lock(channel_context->mutex);
	for (channel_to_websocket = MWS_CHANNEL_TO_WEBSOCKET_READ;
			channel_to_websocket <= MWS_CHANNEL_TO_WEBSOCKET_STEREO;
			channel_to_websocket++) {
		const void *pcm_data = frame->data;
		switch_size_t pcm_length_bytes = frame->datalen;
		uint32_t channels = 2;

		if (channel_to_websocket != MWS_CHANNEL_TO_WEBSOCKET_STEREO) {
			for (sample_index = 0; sample_index < sample_count; sample_index++) {
				if (channel_to_websocket == MWS_CHANNEL_TO_WEBSOCKET_READ) {
					mono_pcm[sample_index] = stereo_pcm[sample_index * 2];
				} else if (channel_to_websocket ==
						MWS_CHANNEL_TO_WEBSOCKET_WRITE) {
					mono_pcm[sample_index] = stereo_pcm[sample_index * 2 + 1];
				} else {
					int32_t mixed_sample = stereo_pcm[sample_index * 2] +
							stereo_pcm[sample_index * 2 + 1];

					switch_normalize_to_16bit(mixed_sample);
					mono_pcm[sample_index] = (int16_t) mixed_sample;
				}
			}
			pcm_data = mono_pcm;
			pcm_length_bytes = sample_count * sizeof(int16_t);
			channels = 1;
		}

		for (stream_context = channel_context->streams; stream_context;
				stream_context = stream_context->channel_next) {
			if (stream_context->channel_to_websocket != channel_to_websocket ||
					atomic_load_explicit(
							&stream_context->shutdown_requested,
							memory_order_acquire)) {
				continue;
			}

			stream_context->captured_media_frame_count++;
			stream_context->captured_native_byte_count += pcm_length_bytes;
			mws_captured_audio_buffer_write(
					&stream_context->captured_audio_buffer,
					pcm_data,
					pcm_length_bytes,
					frame->rate,
					channels);
		}
	}
	switch_mutex_unlock(channel_context->mutex);
}

static void record_playback_callback(
		mws_stream_t *stream_context,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us,
		uint64_t requested_samples,
		mws_playback_state_t playback_state)
{
	if (mws_playback_timing_record_callback(
			&stream_context->playback_timing,
			measurement,
			dispatch_at_us,
			requested_samples,
			playback_state == MWS_PLAYBACK_PLAYING)) {
		mws_playback_timing_record_maximum_gap(
				&stream_context->playback_timing,
				measurement,
				dispatch_at_us,
				mws_source_audio_buffer_queued_bytes(
						&stream_context->source_audio_buffer),
				mws_playback_audio_buffer_queued_bytes(
						&stream_context->playback_audio_buffer),
				(uint32_t) atomic_load_explicit(
					&stream_context->playback_state,
					memory_order_acquire));
	}
}

static void mix_websocket_audio_into_frame(
		mws_stream_t *stream_context,
		switch_frame_t *frame,
		const mws_media_callback_measurement_t *measurement,
		uint64_t dispatch_at_us)
{
	int16_t *frame_pcm;
	switch_size_t frame_size_bytes;
	switch_size_t available_sample_count;
	switch_size_t minimum_buffered_sample_count;
	switch_size_t sample_count;
	uint32_t available_bytes;
	uint32_t playback_buffer_capacity_bytes;
	mws_playback_state_t playback_state;

	if (!frame || !frame->data || !frame->datalen || !frame->rate ||
			!frame->channels ||
			frame->channels > MWS_MAXIMUM_PLAYBACK_CHANNELS) {
		atomic_fetch_add_explicit(
				&stream_context->playback_invalid_frame_count,
				1,
				memory_order_relaxed);
		return;
	}

	frame_size_bytes = frame->channels * sizeof(int16_t);
	if (frame->datalen % frame_size_bytes) {
		atomic_fetch_add_explicit(
				&stream_context->playback_invalid_frame_count,
				1,
				memory_order_relaxed);
		return;
	}

	playback_buffer_capacity_bytes = (uint32_t) calculate_pcm_capacity_bytes(
			frame->rate,
			1,
			MWS_PLAYBACK_BUFFER_DURATION_MS);
	if (!playback_buffer_capacity_bytes || playback_buffer_capacity_bytes >=
			stream_context->playback_audio_buffer.allocation_bytes) {
		atomic_fetch_add_explicit(
				&stream_context->playback_invalid_frame_count,
				1,
				memory_order_relaxed);
		return;
	}

	frame_pcm = (int16_t *) frame->data;
	sample_count = frame->datalen / frame_size_bytes;
	if (frame->samples && sample_count > frame->samples) {
		sample_count = frame->samples;
	}
	playback_state = atomic_load_explicit(
			&stream_context->playback_state,
			memory_order_acquire);
	record_playback_callback(
			stream_context,
			measurement,
			dispatch_at_us,
			sample_count,
			playback_state);

	if (atomic_load_explicit(
			&stream_context->playback_sample_rate,
			memory_order_acquire) != frame->rate) {
		atomic_store_explicit(
				&stream_context->playback_sample_rate,
				frame->rate,
				memory_order_release);
		atomic_fetch_add_explicit(
				&stream_context->playback_format_generation,
				1,
				memory_order_acq_rel);
		atomic_fetch_add_explicit(
				&stream_context->playback_format_change_count,
				1,
				memory_order_relaxed);
		atomic_store_explicit(
				&stream_context->playback_format_reset_requested,
				1,
				memory_order_release);
		atomic_fetch_add_explicit(
				&stream_context->playback_deferred_callback_count,
				1,
				memory_order_relaxed);
		return;
	}

	if (atomic_load_explicit(
			&stream_context->playback_format_reset_requested,
			memory_order_acquire) ||
			!mws_playback_audio_buffer_begin_read(
					&stream_context->playback_audio_buffer,
					&available_bytes)) {
		atomic_fetch_add_explicit(
				&stream_context->playback_deferred_callback_count,
				1,
				memory_order_relaxed);
		return;
	}

	available_sample_count = available_bytes / sizeof(int16_t);

	if (playback_state != MWS_PLAYBACK_PLAYING) {
		minimum_buffered_sample_count = (switch_size_t) (
				((uint64_t) frame->rate * MWS_PLAYBACK_START_BUFFER_MS + 999) /
				1000);
		if (minimum_buffered_sample_count < sample_count) {
			minimum_buffered_sample_count = sample_count;
		}
		if (available_sample_count < minimum_buffered_sample_count) {
			mws_playback_audio_buffer_end_read(
					&stream_context->playback_audio_buffer);
			return;
		}
		atomic_store_explicit(
				&stream_context->playback_state,
				MWS_PLAYBACK_PLAYING,
				memory_order_release);
	} else if (sample_count > available_sample_count) {
		mws_playback_timing_record_underflow(
				&stream_context->playback_timing,
				measurement,
				dispatch_at_us,
				mws_source_audio_buffer_queued_bytes(
						&stream_context->source_audio_buffer),
				available_bytes,
				(uint32_t) playback_state);
		atomic_fetch_add_explicit(
				&stream_context->playback_deferred_callback_count,
				1,
				memory_order_relaxed);
		atomic_store_explicit(
				&stream_context->playback_state,
				MWS_PLAYBACK_BUFFERING,
				memory_order_release);
		mws_playback_audio_buffer_end_read(
				&stream_context->playback_audio_buffer);
		return;
	}

	mws_playback_audio_buffer_mix_and_consume(
			&stream_context->playback_audio_buffer,
			frame_pcm,
			(uint32_t) sample_count,
			frame->channels);
	if (atomic_load_explicit(
			&stream_context->playback_overflow_active,
			memory_order_acquire) &&
			mws_playback_audio_buffer_queued_bytes(
					&stream_context->playback_audio_buffer) <=
			mws_playback_audio_buffer_capacity_bytes(
					&stream_context->playback_audio_buffer) / 2) {
		atomic_store_explicit(
				&stream_context->playback_overflow_active,
				0,
				memory_order_release);
	}
	atomic_fetch_add_explicit(
			&stream_context->playback_mixed_sample_count,
			sample_count,
			memory_order_relaxed);
	mws_playback_audio_buffer_end_read(
			&stream_context->playback_audio_buffer);
}

static switch_bool_t defer_automatic_cleanup_streams(
		mws_channel_t *channel_context)
{
	for (;;) {
		mws_stream_t *stream_context = NULL;
		switch_bool_t last_stream;

		switch_mutex_lock(channel_context->mutex);
		for (stream_context = channel_context->streams; stream_context;
				stream_context = stream_context->channel_next) {
			if (switch_atomic_read(
						&stream_context->automatic_cleanup_requested) &&
					atomic_load_explicit(
							&stream_context->startup_complete,
							memory_order_acquire) &&
					claim_stream_teardown(stream_context)) {
				break;
			}
		}
		switch_mutex_unlock(channel_context->mutex);

		if (!stream_context) {
			return SWITCH_FALSE;
		}

		atomic_store_explicit(
				&stream_context->shutdown_requested,
				true,
				memory_order_release);
		last_stream = detach_stream_from_channel(stream_context);
		release_stream_channel_admission(stream_context);
		defer_claimed_stream_cleanup(stream_context);
		if (last_stream) {
			return SWITCH_TRUE;
		}
	}
}

static uint64_t playback_frame_duration_us(const switch_frame_t *frame)
{
	switch_size_t frame_size_bytes;
	switch_size_t sample_count;

	if (!frame || !frame->datalen || !frame->rate || !frame->channels ||
			frame->channels > MWS_MAXIMUM_PLAYBACK_CHANNELS) {
		return 0;
	}
	frame_size_bytes = frame->channels * sizeof(int16_t);
	if (frame->datalen % frame_size_bytes) {
		return 0;
	}
	sample_count = frame->datalen / frame_size_bytes;
	if (frame->samples && sample_count > frame->samples) {
		sample_count = frame->samples;
	}

	return mws_playback_frame_duration_us(sample_count, frame->rate);
}

static switch_bool_t websocket_stream_media_bug_callback(
		switch_media_bug_t *bug,
		void *user_data,
		switch_abc_type_t type)
{
	mws_channel_t *channel_context = (mws_channel_t *) user_data;
	uint64_t callback_entry_at_us = 0;

	if (type == SWITCH_ABC_TYPE_WRITE_REPLACE ||
			type == SWITCH_ABC_TYPE_READ_REPLACE) {
		callback_entry_at_us = (uint64_t) switch_mono_micro_time_now();
	}
	if (type != SWITCH_ABC_TYPE_INIT &&
			type != SWITCH_ABC_TYPE_CLOSE &&
			defer_automatic_cleanup_streams(channel_context)) {
		return SWITCH_FALSE;
	}

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		switch_mutex_lock(channel_context->mutex);
		channel_context->bug = bug;
		switch_mutex_unlock(channel_context->mutex);
		break;

	case SWITCH_ABC_TYPE_READ_PING:
		{
			uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
			switch_frame_t frame = { 0 };
			switch_bool_t fill = SWITCH_FALSE;
			uint32_t frames_read = 0;

			frame.data = data;
			frame.buflen = sizeof(data);

			while (frames_read < MWS_MAX_MEDIA_FRAMES_PER_CALLBACK &&
					switch_core_media_bug_read(bug, &frame, fill) ==
					SWITCH_STATUS_SUCCESS) {
				if (!frame.datalen) {
					break;
				}

				fan_out_captured_audio(channel_context, &frame);
				frames_read++;
				fill = SWITCH_TRUE;
			}

			if (frames_read == MWS_MAX_MEDIA_FRAMES_PER_CALLBACK) {
				mws_stream_t *stream_context;

				switch_mutex_lock(channel_context->mutex);
				for (stream_context = channel_context->streams; stream_context;
						stream_context = stream_context->channel_next) {
					stream_context->media_callback_budget_hit_count++;
				}
				switch_mutex_unlock(channel_context->mutex);
			}
		}
		break;

	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			switch_frame_t *frame =
					switch_core_media_bug_get_write_replace_frame(bug);
			mws_media_callback_measurement_t measurement =
					mws_media_callback_clock_measure(
							&channel_context->write_playback_clock,
							callback_entry_at_us,
							playback_frame_duration_us(frame));
			mws_stream_t *stream_context;

			switch_mutex_lock(channel_context->mutex);
			for (stream_context = channel_context->streams; stream_context;
					stream_context = stream_context->channel_next) {
				if (!atomic_load_explicit(
						&stream_context->shutdown_requested,
						memory_order_acquire) &&
						stream_context->websocket_to_channel ==
						MWS_WEBSOCKET_TO_CHANNEL_WRITE) {
					mix_websocket_audio_into_frame(
							stream_context,
							frame,
							&measurement,
							(uint64_t) switch_mono_micro_time_now());
				}
			}
			switch_mutex_unlock(channel_context->mutex);
			if (frame) {
				switch_core_media_bug_set_write_replace_frame(bug, frame);
			}
		}
		break;

	case SWITCH_ABC_TYPE_READ_REPLACE:
		{
			switch_frame_t *frame =
					switch_core_media_bug_get_read_replace_frame(bug);
			mws_media_callback_measurement_t measurement =
					mws_media_callback_clock_measure(
							&channel_context->read_playback_clock,
							callback_entry_at_us,
							playback_frame_duration_us(frame));
			mws_stream_t *stream_context;

			switch_mutex_lock(channel_context->mutex);
			for (stream_context = channel_context->streams; stream_context;
					stream_context = stream_context->channel_next) {
				if (!atomic_load_explicit(
						&stream_context->shutdown_requested,
						memory_order_acquire) &&
						stream_context->websocket_to_channel ==
						MWS_WEBSOCKET_TO_CHANNEL_READ) {
					mix_websocket_audio_into_frame(
							stream_context,
							frame,
							&measurement,
							(uint64_t) switch_mono_micro_time_now());
				}
			}
			switch_mutex_unlock(channel_context->mutex);
			if (frame) {
				switch_core_media_bug_set_read_replace_frame(bug, frame);
			}
		}
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			mws_stream_t *all_streams;
			mws_stream_t *deferred_streams = NULL;
			mws_stream_t *stream_context;

			begin_cleanup_callback_producer();
			switch_mutex_lock(channel_context->mutex);
			channel_context->bug = NULL;
			channel_context->closing = SWITCH_TRUE;
			all_streams = channel_context->streams;
			channel_context->streams = NULL;
			for (stream_context = all_streams; stream_context;
					stream_context = stream_context->channel_next) {
				retain_stream_resource_borrow(stream_context);
				stream_context->deferred_cleanup_next = NULL;
				stream_context->close_cleanup_owned = SWITCH_FALSE;
				atomic_store_explicit(
						&stream_context->shutdown_requested,
						true,
						memory_order_release);
				if (claim_stream_teardown(stream_context)) {
					stream_context->close_cleanup_owned = SWITCH_TRUE;
					stream_context->deferred_cleanup_next = deferred_streams;
					deferred_streams = stream_context;
				} else {
					keep_stream_slot_until_cleanup(stream_context);
				}
				stream_context->channel_context = NULL;
			}
			switch_mutex_unlock(channel_context->mutex);

			stream_context = all_streams;
			while (stream_context) {
				mws_stream_t *next_stream = stream_context->channel_next;

				stream_context->channel_next = NULL;
				release_stream_channel_admission(stream_context);
				if (!stream_context->close_cleanup_owned) {
					queue_callback_hold(stream_context);
				}
				stream_context = next_stream;
			}

			stream_context = deferred_streams;
			while (stream_context) {
				mws_stream_t *next_stream =
						stream_context->deferred_cleanup_next;

				stream_context->deferred_cleanup_next = NULL;
				stream_context->close_cleanup_owned = SWITCH_FALSE;
				defer_claimed_stream_cleanup_with_borrow(stream_context);
				stream_context = next_stream;
			}
			end_cleanup_callback_producer();
		}
		break;
	case SWITCH_ABC_TYPE_READ:
	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static void stop_all_streams(void)
{
	for (;;) {
		char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
		switch_core_session_t *session;

		switch_mutex_lock(globals.streams_mutex);

		if (globals.streams) {
			switch_copy_string(
					uuid,
					globals.streams->session_uuid,
					sizeof(uuid));
		}

		switch_mutex_unlock(globals.streams_mutex);

		if (zstr(uuid)) {
			break;
		}

		if ((session = switch_core_session_locate(uuid))) {
			mws_stream_t **stream_contexts = NULL;
			switch_size_t stream_count = 0;

			if (collect_registered_streams(
					uuid,
					NULL,
					&stream_contexts,
					&stream_count) != SWITCH_STATUS_SUCCESS) {
				switch_core_session_rwunlock(session);
				continue;
			}

			(void) stop_registered_streams(
					session,
					stream_contexts,
					stream_count,
					NULL);

			release_registered_streams(&stream_contexts, stream_count);
			switch_core_session_rwunlock(session);
		} else {
			switch_yield(1000);
		}
	}
}

SWITCH_STANDARD_APP(websocket_stream_app)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	mws_channel_t *channel_context;
	mws_stream_t *stream_context;
	const char *channel_to_websocket;
	const char *websocket_to_channel;
	mws_captured_audio_buffer_statistics_t captured_audio_statistics;
	switch_bool_t add_media_bug = SWITCH_FALSE;
	switch_bool_t teardown_owned;
	switch_memory_pool_t *resource_pool = NULL;
	mws_admission_result_t admission_result;
	switch_status_t status;

	switch_channel_set_variable(
			channel,
			MWS_LAST_STARTED_STREAM_ID_VARIABLE,
			NULL);

	if (zstr(data)) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR missing WebSocket URL");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Usage: websocket_stream %s\n",
				MWS_APP_SYNTAX);
		return;
	}

	if (strncasecmp(data, "ws://", 5) &&
			strncasecmp(data, "wss://", 6)) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR WebSocket URL must begin with ws:// or wss://");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"WebSocket URL must begin with ws:// or wss://\n");
		return;
	}
	if (mws_bounded_string_length(data, MWS_MAXIMUM_URL_BYTES) >
			MWS_MAXIMUM_URL_BYTES) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR WebSocket URL exceeds 4096 bytes");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"WebSocket URL exceeds %u bytes\n",
				MWS_MAXIMUM_URL_BYTES);
		return;
	}

	if (!switch_channel_media_up(channel) ||
			!switch_core_session_get_read_codec(session)) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR media is not established");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Cannot start WebSocket stream before media is established\n");
		return;
	}

	admission_result = reserve_stream_admission(session, &channel_context);
	if (admission_result != MWS_ADMISSION_OK) {
		const char *response =
				admission_result == MWS_ADMISSION_GLOBAL_LIMIT ?
				"-ERR WebSocket stream limit reached" :
				admission_result == MWS_ADMISSION_CHANNEL_LIMIT ?
				"-ERR channel WebSocket stream limit reached" :
				"-ERR could not initialize channel media context";

		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				response);
		return;
	}

	if (switch_core_new_memory_pool(&resource_pool) != SWITCH_STATUS_SUCCESS) {
		release_channel_admission(channel_context);
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not allocate WebSocket stream");
		return;
	}
	stream_context = switch_core_alloc(resource_pool, sizeof(*stream_context));
	if (!stream_context) {
		release_channel_admission(channel_context);
		switch_core_destroy_memory_pool(&resource_pool);
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not allocate WebSocket stream");
		return;
	}
	memset(stream_context, 0, sizeof(*stream_context));
	stream_context->resource_pool = resource_pool;
	stream_context->resource_borrow_count = 1;
	stream_context->admission_reserved = SWITCH_TRUE;
	stream_context->admission_channel_context = channel_context;

	stream_context->url = switch_core_strdup(
			resource_pool,
			data);
	stream_context->session_uuid = switch_core_strdup(
			resource_pool,
			switch_core_session_get_uuid(session));
	if (!stream_context->url || !stream_context->session_uuid) {
		release_stream_admission(stream_context);
		request_stream_resource_release(stream_context);
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not allocate WebSocket stream");
		release_stream_resource_borrow(stream_context);
		return;
	}
	switch_uuid_str(
			stream_context->stream_id,
			sizeof(stream_context->stream_id));

	switch_mutex_lock(globals.config_mutex);

	stream_context->target_sample_rate = globals.target_sample_rate;
	stream_context->channel_to_websocket = globals.channel_to_websocket;
	stream_context->websocket_to_channel = globals.websocket_to_channel;
	stream_context->reconnect_attempt_limit = globals.reconnect_attempts;
	stream_context->reconnect_delay_ms = globals.reconnect_delay_ms;
	stream_context->statistics_interval_ms = globals.statistics_interval_ms;
	stream_context->maximum_start_variables =
			(uint32_t) globals.maximum_start_variables;
	stream_context->maximum_start_variable_bytes =
			(switch_size_t) globals.maximum_start_variable_bytes;
	stream_context->maximum_start_message_bytes =
			(switch_size_t) globals.maximum_start_message_bytes;
	stream_context->maximum_api_response_bytes =
			(switch_size_t) globals.maximum_api_response_bytes;
	stream_context->api_commands_enabled = globals.api_commands_enabled;

	switch_mutex_unlock(globals.config_mutex);

	/* The channel variable may only narrow the configured setting. */
	if (switch_false(switch_channel_get_variable(
			channel,
			MWS_API_COMMANDS_VARIABLE))) {
		stream_context->api_commands_enabled = SWITCH_FALSE;
	}

	if (initialize_stream_context(session, stream_context) !=
			SWITCH_STATUS_SUCCESS) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not initialize WebSocket stream");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not initialize WebSocket stream context\n");
		release_stream_admission(stream_context);
		request_stream_resource_release(stream_context);
		release_stream_resource_borrow(stream_context);
		return;
	}

	if (build_stream_request_headers(session, stream_context) !=
			SWITCH_STATUS_SUCCESS) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not build WebSocket request headers");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not build WebSocket HTTP request headers\n");
		release_stream_admission(stream_context);
		request_stream_resource_release(stream_context);
		release_stream_resource_borrow(stream_context);
		return;
	}

	if (build_start_message(session, stream_context) !=
			SWITCH_STATUS_SUCCESS) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not build WebSocket start message");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not build WebSocket start message\n");
		release_stream_admission(stream_context);
		request_stream_resource_release(stream_context);
		release_stream_resource_borrow(stream_context);
		return;
	}

	register_stream(stream_context);

	status = start_stream_worker(stream_context);
	if (status != SWITCH_STATUS_SUCCESS) {
		teardown_owned = claim_stream_teardown(stream_context);
		atomic_store_explicit(
				&stream_context->shutdown_requested,
				true,
				memory_order_release);
		(void) complete_stream_startup(stream_context);
		if (teardown_owned) {
			stop_stream_worker(stream_context);
			unregister_stream(stream_context);
		}
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not attach stream to transport worker");

		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not attach stream to transport worker\n");
		release_stream_resource_borrow(stream_context);
		return;
	}

	status = attach_stream_to_channel(channel_context, stream_context);
	if (status != SWITCH_STATUS_SUCCESS) {
		teardown_owned = claim_stream_teardown(stream_context);
		atomic_store_explicit(
				&stream_context->shutdown_requested,
				true,
				memory_order_release);
		(void) complete_stream_startup(stream_context);
		if (teardown_owned) {
			stop_stream_worker(stream_context);
			unregister_stream(stream_context);
		}
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR channel media context is closing");
		release_stream_resource_borrow(stream_context);
		return;
	}

	switch_mutex_lock(channel_context->mutex);
	if (!channel_context->bug &&
			!channel_context->media_bug_add_in_progress) {
		channel_context->media_bug_add_in_progress = SWITCH_TRUE;
		add_media_bug = SWITCH_TRUE;
	}
	switch_mutex_unlock(channel_context->mutex);

	if (add_media_bug) {
		status = switch_core_media_bug_add(
				session,
				"websocket_stream",
				channel_context->session_uuid,
				websocket_stream_media_bug_callback,
				channel_context,
				0,
				media_bug_flags(),
				&channel_context->bug);

		switch_mutex_lock(channel_context->mutex);
		channel_context->media_bug_add_in_progress = SWITCH_FALSE;
		switch_thread_cond_broadcast(channel_context->condition);
		switch_mutex_unlock(channel_context->mutex);
	} else {
		status = SWITCH_STATUS_SUCCESS;
	}

	if (status != SWITCH_STATUS_SUCCESS) {
		teardown_owned = claim_stream_teardown(stream_context);
		atomic_store_explicit(
				&stream_context->shutdown_requested,
				true,
				memory_order_release);
		(void) complete_stream_startup(stream_context);
		if (teardown_owned) {
			(void) detach_stream_from_channel(stream_context);
			stop_stream_worker(stream_context);
			unregister_stream(stream_context);
			reopen_empty_channel_context(channel_context);
		}
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not attach WebSocket media bug");

		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not attach WebSocket media bug\n");
		release_stream_resource_borrow(stream_context);
		return;
	}

	if (!complete_stream_startup(stream_context)) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR channel closed while WebSocket stream was starting");
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_WARNING,
				"Channel closed while WebSocket stream was starting: "
				"stream_id=%s\n",
				stream_context->stream_id);
		release_stream_resource_borrow(stream_context);
		return;
	}

	channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			stream_context->channel_to_websocket);
	websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			stream_context->websocket_to_channel);
	mws_captured_audio_buffer_snapshot(
			&stream_context->captured_audio_buffer,
			&captured_audio_statistics);

	switch_channel_set_variable(
			channel,
			MWS_LAST_STARTED_STREAM_ID_VARIABLE,
			stream_context->stream_id);
	switch_channel_set_variable_printf(
			channel,
			SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
			"+OK WebSocket stream starting stream_id=%s",
			stream_context->stream_id);

	switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_NOTICE,
			"WebSocket stream attached: stream_id=%s "
			"target_rate_hz=%d channel_to_websocket=%s "
			"websocket_to_channel=%s "
			"reconnect_attempt_limit=%d reconnect_delay_ms=%d "
			"native_rate_hz=%u native_channels=%u "
			"native_ring_capacity_bytes=%" SWITCH_SIZE_T_FMT "\n",
			stream_context->stream_id,
			stream_context->target_sample_rate,
			channel_to_websocket ? channel_to_websocket : "unknown",
			websocket_to_channel ? websocket_to_channel : "unknown",
			stream_context->reconnect_attempt_limit,
			stream_context->reconnect_delay_ms,
			captured_audio_statistics.sample_rate,
			captured_audio_statistics.channels,
			captured_audio_statistics.capacity_bytes);
	release_stream_resource_borrow(stream_context);
}

static switch_status_t stop_registered_streams(
		switch_core_session_t *session,
		mws_stream_t **stream_contexts,
		switch_size_t stream_count,
		switch_size_t *stopped_count)
{
	mws_channel_t *channel_context = NULL;
	mws_channel_t *empty_channel_context = NULL;
	switch_media_bug_t *bug;
	switch_bool_t *claimed;
	switch_size_t index;
	switch_status_t overall_status = SWITCH_STATUS_SUCCESS;

	if (stopped_count) {
		*stopped_count = 0;
	}
	if (!stream_count) {
		return SWITCH_STATUS_SUCCESS;
	}
	claimed = calloc(stream_count, sizeof(*claimed));
	if (!claimed) {
		return SWITCH_STATUS_MEMERR;
	}

	for (index = 0; index < stream_count; index++) {
		mws_stream_t *stream_context = stream_contexts[index];

		if (!claim_stream_teardown(stream_context)) {
			continue;
		}
		claimed[index] = SWITCH_TRUE;
		atomic_store_explicit(
				&stream_context->shutdown_requested,
				true,
				memory_order_release);
		if (!channel_context && stream_context->channel_context) {
			channel_context = stream_context->channel_context;
		}
	}

	if (channel_context) {
		switch_mutex_lock(channel_context->mutex);
		while (channel_context->media_bug_add_in_progress) {
			switch_thread_cond_wait(
					channel_context->condition,
					channel_context->mutex);
		}
		switch_mutex_unlock(channel_context->mutex);
	}

	for (index = 0; index < stream_count; index++) {
		mws_stream_t *stream_context = stream_contexts[index];
		mws_channel_t *stream_channel_context =
				stream_context->channel_context;

		if (!claimed[index]) {
			continue;
		}

		if (detach_stream_from_channel(stream_context)) {
			empty_channel_context = stream_channel_context;
		}
		if (request_stream_worker_stop(stream_context) !=
				SWITCH_STATUS_SUCCESS) {
			overall_status = SWITCH_STATUS_FALSE;
		}
	}

	for (index = 0; index < stream_count; index++) {
		mws_stream_t *stream_context = stream_contexts[index];

		if (!claimed[index]) {
			continue;
		}

		if (wait_for_stream_worker_stop(stream_context) !=
				SWITCH_STATUS_SUCCESS) {
			overall_status = SWITCH_STATUS_FALSE;
			continue;
		}
		if (stream_is_registered(stream_context)) {
			unregister_stream(stream_context);
		}
		if (stopped_count) {
			(*stopped_count)++;
		}
	}

	if (empty_channel_context) {
		switch_mutex_lock(empty_channel_context->mutex);
		bug = empty_channel_context->bug;
		switch_mutex_unlock(empty_channel_context->mutex);
		if (bug) {
			if (switch_core_media_bug_remove(session, &bug) !=
					SWITCH_STATUS_SUCCESS) {
				overall_status = SWITCH_STATUS_FALSE;
			} else {
				reopen_empty_channel_context(empty_channel_context);
			}
		} else {
			reopen_empty_channel_context(empty_channel_context);
		}
	}

	switch_safe_free(claimed);
	return overall_status;
}

static switch_status_t stop_registered_stream(
		switch_core_session_t *session,
		mws_stream_t *stream_context)
{
	mws_stream_t *stream_contexts[] = {stream_context};

	return stop_registered_streams(
			session,
			stream_contexts,
			1,
			NULL);
}

SWITCH_STANDARD_APP(websocket_stream_stop_app)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	mws_stream_t **stream_contexts = NULL;
	switch_size_t stream_count = 0;
	const char *stream_id = zstr(data) || !strcasecmp(data, "all") ?
			NULL : data;
	switch_size_t stopped_count = 0;
	switch_status_t stop_status;

	if (collect_registered_streams(
				switch_core_session_get_uuid(session),
				stream_id,
				&stream_contexts,
				&stream_count) != SWITCH_STATUS_SUCCESS) {
		switch_channel_set_variable(
				channel,
				SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
				"-ERR could not inspect WebSocket streams");
		return;
	}

	if (!stream_count) {
		if (zstr(stream_id)) {
			switch_channel_set_variable(
					channel,
					SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
					"-ERR no WebSocket stream is active");
		} else {
			switch_channel_set_variable_printf(
					channel,
					SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
					"-ERR WebSocket stream %s was not found",
					stream_id);
		}
		release_registered_streams(&stream_contexts, stream_count);
		return;
	}

	stop_status = stop_registered_streams(
			session,
			stream_contexts,
			stream_count,
			&stopped_count);
	if (stop_status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Could not stop every WebSocket stream cleanly\n");
	}
	release_registered_streams(&stream_contexts, stream_count);

	switch_channel_set_variable_printf(
			channel,
			SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE,
			stopped_count == stream_count ?
					"+OK stopped_streams=%" SWITCH_SIZE_T_FMT :
					"-ERR stopped_streams=%" SWITCH_SIZE_T_FMT
					" failed_streams=%" SWITCH_SIZE_T_FMT,
			stopped_count,
			stream_count - stopped_count);
	switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_NOTICE,
			"Stopped %" SWITCH_SIZE_T_FMT
			" WebSocket stream(s); channel remains active\n",
			stopped_count);
}

static const char *queue_outbound_message(
		mws_stream_t *stream_context,
		const char *message,
		switch_size_t message_length)
{
	mws_transport_state_t transport_state;
	switch_size_t slot;

	if (atomic_load_explicit(
			&stream_context->shutdown_requested,
			memory_order_acquire)) {
		return "stream is stopping";
	}
	switch_mutex_lock(globals.worker_mutex);
	transport_state = stream_context->transport_state;
	switch_mutex_unlock(globals.worker_mutex);
	if (transport_state != MWS_TRANSPORT_OPEN) {
		return "stream is not connected";
	}

	switch_mutex_lock(stream_context->outbound_message_mutex);
	if (stream_context->outbound_message_count >=
			MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY) {
		switch_mutex_unlock(stream_context->outbound_message_mutex);
		return "outbound message queue is full";
	}
	slot = stream_context->outbound_message_write_index;
	memcpy(stream_context->outbound_messages[slot], message, message_length);
	stream_context->outbound_message_lengths[slot] = message_length;
	stream_context->outbound_message_write_index =
			(slot + 1) % MWS_OUTBOUND_MESSAGE_QUEUE_CAPACITY;
	stream_context->outbound_message_count++;
	switch_mutex_unlock(stream_context->outbound_message_mutex);

	return NULL;
}

/* <channel-uuid> send <stream-id|all> <json>; the JSON may contain spaces. */
static void uuid_websocket_stream_send(
		const char *cmd,
		switch_stream_handle_t *stream)
{
	char *command_copy = NULL;
	char *channel_uuid;
	const char *target;
	char *message;
	char *cursor;
	switch_size_t message_length;
	cJSON *document = NULL;
	switch_core_session_t *target_session = NULL;
	mws_stream_t **stream_contexts = NULL;
	switch_size_t stream_count = 0;
	switch_size_t sent_stream_count = 0;
	const char *last_error = NULL;
	switch_size_t index;

	if (!(command_copy = strdup(cmd))) {
		stream->write_function(stream, "-ERR could not allocate command\n");
		return;
	}

	cursor = command_copy;
	while (*cursor && isspace((unsigned char) *cursor)) {
		cursor++;
	}
	channel_uuid = cursor;
	while (*cursor && !isspace((unsigned char) *cursor)) {
		cursor++;
	}
	while (*cursor && isspace((unsigned char) *cursor)) {
		*cursor++ = '\0';
	}
	while (*cursor && !isspace((unsigned char) *cursor)) {
		cursor++; /* the "send" keyword */
	}
	while (*cursor && isspace((unsigned char) *cursor)) {
		*cursor++ = '\0';
	}
	/* Stream IDs never begin with '{', so a missing target means all. */
	if (*cursor == '{') {
		target = "all";
	} else {
		target = cursor;
		while (*cursor && !isspace((unsigned char) *cursor)) {
			cursor++;
		}
		while (*cursor && isspace((unsigned char) *cursor)) {
			*cursor++ = '\0';
		}
	}
	message = cursor;
	message_length = strlen(message);
	while (message_length &&
			isspace((unsigned char) message[message_length - 1])) {
		message[--message_length] = '\0';
	}

	if (zstr(channel_uuid) || zstr(target) || !message_length) {
		stream->write_function(
				stream,
				"-USAGE: uuid_websocket_stream %s\n",
				MWS_UUID_API_SYNTAX);
		goto done;
	}
	if (message_length > MWS_MAXIMUM_TEXT_MESSAGE_BYTES) {
		stream->write_function(
				stream,
				"-ERR messages may not exceed %u bytes\n",
				(unsigned int) MWS_MAXIMUM_TEXT_MESSAGE_BYTES);
		goto done;
	}
	if (!mws_websocket_text_is_valid_utf8(
				(const uint8_t *) message,
				message_length) ||
			!(document = cJSON_Parse(message)) ||
			!cJSON_IsObject(document)) {
		stream->write_function(
				stream,
				"-ERR message must be a UTF-8 JSON object\n");
		goto done;
	}

	target_session = switch_core_session_locate(channel_uuid);
	if (!target_session) {
		stream->write_function(
				stream,
				"-ERR channel %s was not found\n",
				channel_uuid);
		goto done;
	}
	if (collect_registered_streams(
			channel_uuid,
			strcasecmp(target, "all") ? target : NULL,
			&stream_contexts,
			&stream_count) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(
				stream,
				"-ERR could not inspect WebSocket streams\n");
		goto done;
	}
	if (!stream_count) {
		stream->write_function(
				stream,
				"-ERR no matching WebSocket stream on channel %s\n",
				channel_uuid);
		goto done;
	}

	for (index = 0; index < stream_count; index++) {
		const char *error = queue_outbound_message(
				stream_contexts[index],
				message,
				message_length);

		if (error) {
			last_error = error;
		} else {
			sent_stream_count++;
		}
	}
	if (sent_stream_count == stream_count) {
		stream->write_function(
				stream,
				"+OK queued_streams=%" SWITCH_SIZE_T_FMT "\n",
				sent_stream_count);
	} else {
		stream->write_function(
				stream,
				"-ERR %s queued_streams=%" SWITCH_SIZE_T_FMT
				" failed_streams=%" SWITCH_SIZE_T_FMT "\n",
				last_error,
				sent_stream_count,
				stream_count - sent_stream_count);
	}

done:
	release_registered_streams(&stream_contexts, stream_count);
	if (target_session) {
		switch_core_session_rwunlock(target_session);
	}
	if (document) {
		cJSON_Delete(document);
	}
	switch_safe_free(command_copy);
}

static switch_bool_t command_is_send(const char *cmd)
{
	const char *cursor = cmd;

	while (*cursor && isspace((unsigned char) *cursor)) {
		cursor++;
	}
	while (*cursor && !isspace((unsigned char) *cursor)) {
		cursor++;
	}
	while (*cursor && isspace((unsigned char) *cursor)) {
		cursor++;
	}

	return !strncasecmp(cursor, "send", 4) &&
			(!cursor[4] || isspace((unsigned char) cursor[4]));
}

SWITCH_STANDARD_API(uuid_websocket_stream_api)
{
	char *command_copy = NULL;
	char *arguments[5] = { 0 };
	int argument_count;
	switch_core_session_t *target_session = NULL;
	mws_stream_t **stream_contexts = NULL;
	switch_size_t stream_count = 0;
	switch_size_t index;
	uint64_t total_discarded_sample_count = 0;
	switch_size_t cleared_stream_count = 0;
	const char *stream_id = NULL;
	switch_bool_t status_requested = SWITCH_FALSE;
	switch_bool_t json_requested = SWITCH_FALSE;
	switch_bool_t valid_command = SWITCH_FALSE;

	(void) session;

	if (zstr(cmd) || !(command_copy = strdup(cmd))) {
		stream->write_function(
				stream,
				"-USAGE: uuid_websocket_stream %s\n",
				MWS_UUID_API_SYNTAX);
		goto done;
	}

	if (command_is_send(cmd)) {
		uuid_websocket_stream_send(cmd, stream);
		goto done;
	}

	argument_count = switch_separate_string(
			command_copy,
			' ',
			arguments,
			5);
	if (argument_count >= 2 && !zstr(arguments[0]) &&
			!strcasecmp(arguments[1], "status")) {
		status_requested = SWITCH_TRUE;
		if (argument_count == 2) {
			valid_command = SWITCH_TRUE;
		} else if (argument_count == 3) {
			if (!strcasecmp(arguments[2], "json")) {
				json_requested = SWITCH_TRUE;
			} else if (!zstr(arguments[2])) {
				stream_id = arguments[2];
			}
			valid_command = SWITCH_TRUE;
		} else if (argument_count == 4 &&
				!zstr(arguments[2]) &&
				strcasecmp(arguments[2], "json") &&
				!strcasecmp(arguments[3], "json")) {
			stream_id = arguments[2];
			json_requested = SWITCH_TRUE;
			valid_command = SWITCH_TRUE;
		}
	} else if (argument_count >= 2 && !zstr(arguments[0]) &&
			!strcasecmp(arguments[1], "clear") &&
			(argument_count == 2 ||
			 (argument_count == 3 &&
			  !zstr(arguments[2]) &&
			  strcasecmp(arguments[2], "json")))) {
		stream_id = argument_count == 3 ? arguments[2] : NULL;
		valid_command = SWITCH_TRUE;
	}
	if (!valid_command) {
		if (argument_count > 0 &&
				!strcasecmp(arguments[argument_count - 1], "json")) {
			write_json_error(
					stream,
					"usage",
					"usage",
					MWS_UUID_API_SYNTAX);
		} else {
			stream->write_function(
					stream,
					"-USAGE: uuid_websocket_stream %s\n",
					MWS_UUID_API_SYNTAX);
		}
		goto done;
	}

	target_session = switch_core_session_locate(arguments[0]);
	if (!target_session) {
		if (json_requested) {
			write_json_error(
					stream,
					"channel_not_found",
					"channel_uuid",
					arguments[0]);
		} else {
			stream->write_function(
					stream,
					"-ERR channel %s was not found\n",
					arguments[0]);
		}
		goto done;
	}

	if (collect_registered_streams(
			arguments[0],
			stream_id,
			&stream_contexts,
			&stream_count) != SWITCH_STATUS_SUCCESS) {
		if (json_requested) {
			write_json_error(stream, "inspection_failed", NULL, NULL);
		} else {
			stream->write_function(
					stream,
					"-ERR could not inspect WebSocket streams\n");
		}
		goto done;
	}
	if (!stream_count) {
		if (json_requested) {
			write_json_error(
					stream,
					zstr(stream_id) ? "no_streams" : "stream_not_found",
					zstr(stream_id) ? "channel_uuid" : "stream_id",
					zstr(stream_id) ? arguments[0] : stream_id);
		} else {
			if (zstr(stream_id)) {
				stream->write_function(
						stream,
						"-ERR channel %s has no WebSocket stream\n",
						arguments[0]);
			} else {
				stream->write_function(
						stream,
						"-ERR stream %s was not found on channel %s\n",
						stream_id,
						arguments[0]);
			}
		}
		goto done;
	}

	if (status_requested) {
		if (json_requested) {
			cJSON *document = cJSON_CreateObject();
			cJSON *streams = cJSON_CreateArray();
			switch_status_t json_status = SWITCH_STATUS_SUCCESS;

			if (!document || !streams ||
					!cJSON_AddBoolToObject(document, "ok", 1) ||
					!cJSON_AddNumberToObject(document, "version", 1) ||
					!cJSON_AddStringToObject(
						document,
						"channel_uuid",
						arguments[0]) ||
					!cJSON_AddNumberToObject(
						document,
						"stream_count",
						(double) stream_count)) {
				json_status = SWITCH_STATUS_MEMERR;
			}
			for (index = 0;
					json_status == SWITCH_STATUS_SUCCESS &&
					index < stream_count;
					index++) {
				json_status = add_stream_status_json(
						streams,
						stream_contexts[index]);
			}
			if (json_status == SWITCH_STATUS_SUCCESS) {
				cJSON_AddItemToObject(document, "streams", streams);
				streams = NULL;
				json_status = write_json_document(stream, document);
			}
			if (json_status != SWITCH_STATUS_SUCCESS) {
				stream->write_function(
						stream,
						"-ERR could not build JSON status\n");
			}
			if (streams) {
				cJSON_Delete(streams);
			}
			if (document) {
				cJSON_Delete(document);
			}
		} else {
			stream->write_function(
					stream,
					"+OK streams=%" SWITCH_SIZE_T_FMT "\n",
					stream_count);
			for (index = 0; index < stream_count; index++) {
				write_stream_status(stream, stream_contexts[index]);
			}
		}
		goto done;
	}

	for (index = 0; index < stream_count; index++) {
		uint64_t discarded_sample_count = 0;

		if (clear_stream_worker(
				stream_contexts[index],
				&discarded_sample_count) != SWITCH_STATUS_SUCCESS) {
			continue;
		}
		total_discarded_sample_count += discarded_sample_count;
		cleared_stream_count++;
	}
	stream->write_function(
			stream,
			cleared_stream_count == stream_count ? "+OK" : "-ERR");
	stream->write_function(
			stream,
			" cleared_streams=%" SWITCH_SIZE_T_FMT
			" failed_streams=%" SWITCH_SIZE_T_FMT
			" cleared_samples=%" SWITCH_UINT64_T_FMT "\n",
			cleared_stream_count,
			stream_count - cleared_stream_count,
			total_discarded_sample_count);

done:
	release_registered_streams(&stream_contexts, stream_count);
	if (target_session) {
		switch_core_session_rwunlock(target_session);
	}
	switch_safe_free(command_copy);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(websocket_stream_api)
{
	const curl_version_info_data *curl_info;
	const char *channel_to_websocket;
	const char *websocket_to_channel;
	int target_sample_rate;
	int reconnect_attempts;
	int reconnect_delay_ms;
	int statistics_interval_ms;
	int configured_transport_workers;
	int maximum_streams;
	int maximum_streams_per_channel;
	int maximum_http_headers;
	int maximum_http_header_bytes;
	int maximum_start_variables;
	int maximum_start_variable_bytes;
	int maximum_start_message_bytes;
	int maximum_api_response_bytes;
	switch_bool_t api_commands_enabled;
	mws_channel_to_websocket_t channel_to_websocket_value;
	mws_websocket_to_channel_t websocket_to_channel_value;
	uint32_t configured_header_count;
	uint32_t active_streams;
	uint32_t reserved_streams;
	uint32_t connecting_streams;
	uint32_t open_streams;
	uint32_t reconnecting_streams;
	uint32_t failed_streams;
	uint64_t reconnect_attempt_count;
	uint64_t reconnect_success_count;
	uint64_t reconnect_exhausted_count;
	uint64_t transport_event_count;
	uint64_t statistics_event_count;
	uint64_t error_event_count;
	uint64_t message_event_count;
	uint64_t automatic_cleanup_request_count;
	unsigned int worker_queue_size;
	uint32_t transport_workers;
	uint32_t transport_workers_running;
	uint32_t transport_workers_used;
	uint32_t transport_worker_max_streams;
	unsigned int api_command_queue_size;
	unsigned int cleanup_queue_size;
	switch_bool_t worker_running;
	switch_bool_t api_command_worker_running;
	switch_bool_t cleanup_worker_running;
	switch_bool_t json_requested = SWITCH_FALSE;

	(void) session;

	if (!zstr(cmd) && !strcasecmp(cmd, "reload")) {
		if (load_config(SWITCH_TRUE) != SWITCH_STATUS_SUCCESS) {
			stream->write_function(
					stream,
					"-ERR configuration reload failed\n");

			return SWITCH_STATUS_SUCCESS;
		}

		stream->write_function(
				stream,
				"+OK configuration reloaded\n");

		return SWITCH_STATUS_SUCCESS;
	}

	if (!zstr(cmd) && !strcasecmp(cmd, "status json")) {
		json_requested = SWITCH_TRUE;
	} else if (zstr(cmd) || strcasecmp(cmd, "status")) {
		stream->write_function(
			stream,
			"-USAGE: websocket_stream %s\n",
			MWS_API_SYNTAX);

		return SWITCH_STATUS_SUCCESS;
	}

	curl_info = curl_version_info(CURLVERSION_NOW);

	switch_mutex_lock(globals.config_mutex);

	target_sample_rate = globals.target_sample_rate;
	channel_to_websocket_value = globals.channel_to_websocket;
	websocket_to_channel_value = globals.websocket_to_channel;
	reconnect_attempts = globals.reconnect_attempts;
	reconnect_delay_ms = globals.reconnect_delay_ms;
	configured_header_count = globals.configured_header_count;
	statistics_interval_ms = globals.statistics_interval_ms;
	configured_transport_workers = globals.configured_transport_workers;
	maximum_streams = globals.maximum_streams;
	maximum_streams_per_channel = globals.maximum_streams_per_channel;
	maximum_http_headers = globals.maximum_http_headers;
	maximum_http_header_bytes = globals.maximum_http_header_bytes;
	maximum_start_variables = globals.maximum_start_variables;
	maximum_start_variable_bytes = globals.maximum_start_variable_bytes;
	maximum_start_message_bytes = globals.maximum_start_message_bytes;
	maximum_api_response_bytes = globals.maximum_api_response_bytes;
	api_commands_enabled = globals.api_commands_enabled;

	switch_mutex_unlock(globals.config_mutex);

	channel_to_websocket = switch_xml_config_enum_int2str(
			channel_to_websocket_options,
			channel_to_websocket_value);
	websocket_to_channel = switch_xml_config_enum_int2str(
			websocket_to_channel_options,
			websocket_to_channel_value);

	switch_mutex_lock(globals.streams_mutex);
	active_streams = globals.active_streams;
	reserved_streams = globals.reserved_streams;
	switch_mutex_unlock(globals.streams_mutex);

	transport_workers = globals.transport_worker_count;
	transport_workers_running = 0;
	transport_workers_used = 0;
	transport_worker_max_streams = 0;
	worker_queue_size = 0;
	{
		uint32_t index;

		for (index = 0; index < transport_workers; index++) {
			mws_transport_worker_t *worker =
					&globals.transport_workers[index];
			uint32_t assigned_streams;

			switch_mutex_lock(worker->mutex);
			if (worker->running) {
				transport_workers_running++;
			}
			assigned_streams = worker->assigned_streams;
			switch_mutex_unlock(worker->mutex);
			if (assigned_streams) {
				transport_workers_used++;
			}
			if (assigned_streams > transport_worker_max_streams) {
				transport_worker_max_streams = assigned_streams;
			}
			worker_queue_size += switch_queue_size(worker->commands);
		}
	}
	worker_running = transport_workers_running == transport_workers;
	switch_mutex_lock(globals.worker_mutex);
	connecting_streams = globals.connecting_streams;
	open_streams = globals.open_streams;
	reconnecting_streams = globals.reconnecting_streams;
	failed_streams = globals.failed_streams;
	reconnect_attempt_count = globals.reconnect_attempt_count;
	reconnect_success_count = globals.reconnect_success_count;
	reconnect_exhausted_count = globals.reconnect_exhausted_count;
	transport_event_count = globals.transport_event_count;
	statistics_event_count = globals.statistics_event_count;
	error_event_count = globals.error_event_count;
	message_event_count = globals.message_event_count;
	automatic_cleanup_request_count =
			globals.automatic_cleanup_request_count;
	switch_mutex_unlock(globals.worker_mutex);

	switch_mutex_lock(globals.api_command_worker_mutex);
	api_command_worker_running = globals.api_command_worker_running;
	switch_mutex_unlock(globals.api_command_worker_mutex);
	api_command_queue_size = switch_queue_size(globals.api_command_tasks);
	switch_mutex_lock(globals.cleanup_worker_mutex);
	cleanup_worker_running = globals.cleanup_worker_running;
	cleanup_queue_size = switch_queue_size(globals.cleanup_tasks) +
			globals.pending_detached_cleanup_count +
			globals.pending_callback_hold_count;
	switch_mutex_unlock(globals.cleanup_worker_mutex);

	if (json_requested) {
		cJSON *document = cJSON_CreateObject();
		switch_status_t json_status = SWITCH_STATUS_SUCCESS;

		if (!document ||
				!cJSON_AddBoolToObject(document, "ok", 1) ||
				!cJSON_AddNumberToObject(document, "version", 1) ||
				!cJSON_AddStringToObject(
					document,
					"module",
					"mod_websocket_stream") ||
				!cJSON_AddStringToObject(document, "state", "loaded") ||
				!cJSON_AddStringToObject(document, "build_id", MWS_BUILD_ID) ||
				!cJSON_AddStringToObject(
					document,
					"curl_version",
					curl_info ? curl_info->version : "unknown") ||
				!cJSON_AddNumberToObject(
					document,
					"target_rate_hz",
					target_sample_rate) ||
				!cJSON_AddStringToObject(
					document,
					"channel_to_websocket",
					channel_to_websocket ? channel_to_websocket : "unknown") ||
				!cJSON_AddStringToObject(
					document,
					"websocket_to_channel",
					websocket_to_channel ? websocket_to_channel : "unknown") ||
				!cJSON_AddNumberToObject(
					document,
					"reconnect_attempts",
					reconnect_attempts) ||
				!cJSON_AddNumberToObject(
					document,
					"reconnect_delay_ms",
					reconnect_delay_ms) ||
				!cJSON_AddNumberToObject(
					document,
					"configured_headers",
					configured_header_count) ||
				!cJSON_AddNumberToObject(
					document,
					"statistics_interval_ms",
					statistics_interval_ms) ||
				!cJSON_AddNumberToObject(
					document,
					"configured_transport_workers",
					configured_transport_workers) ||
				!cJSON_AddNumberToObject(
					document,
					"transport_workers",
					transport_workers) ||
				!cJSON_AddNumberToObject(
					document,
					"transport_workers_running",
					transport_workers_running) ||
				!cJSON_AddNumberToObject(
					document,
					"transport_workers_used",
					transport_workers_used) ||
				!cJSON_AddNumberToObject(
					document,
					"transport_worker_max_streams",
					transport_worker_max_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_transport_workers",
					MWS_HARD_MAXIMUM_TRANSPORT_WORKERS) ||
				!cJSON_AddNumberToObject(
					document,
					"active_streams",
					active_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"reserved_streams",
					reserved_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_streams",
					maximum_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_streams_per_channel",
					maximum_streams_per_channel) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_http_headers",
					maximum_http_headers) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_http_header_bytes",
					maximum_http_header_bytes) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_start_variables",
					maximum_start_variables) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_start_variable_bytes",
					maximum_start_variable_bytes) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_start_message_bytes",
					maximum_start_message_bytes) ||
				!cJSON_AddNumberToObject(
					document,
					"maximum_api_response_bytes",
					maximum_api_response_bytes) ||
				!cJSON_AddBoolToObject(
					document,
					"api_commands",
					api_commands_enabled ? 1 : 0) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_streams",
					MWS_HARD_MAXIMUM_STREAMS) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_streams_per_channel",
					MWS_HARD_MAXIMUM_STREAMS_PER_CHANNEL) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_http_headers",
					MWS_HARD_MAXIMUM_HTTP_HEADERS) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_http_header_bytes",
					MWS_HARD_MAXIMUM_HTTP_HEADER_BYTES) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_start_variables",
					MWS_HARD_MAXIMUM_START_VARIABLES) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_start_variable_bytes",
					MWS_HARD_MAXIMUM_START_VARIABLE_BYTES) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_start_message_bytes",
					MWS_HARD_MAXIMUM_START_MESSAGE_BYTES) ||
				!cJSON_AddNumberToObject(
					document,
					"hard_maximum_api_response_bytes",
					MWS_HARD_MAXIMUM_API_RESPONSE_BYTES) ||
				!cJSON_AddStringToObject(
					document,
					"worker",
					worker_running ? "running" : "stopped") ||
				!cJSON_AddNumberToObject(
					document,
					"worker_queue",
					worker_queue_size) ||
				!cJSON_AddStringToObject(
					document,
					"api_worker",
					api_command_worker_running ? "running" : "stopped") ||
				!cJSON_AddNumberToObject(
					document,
					"api_queue",
					api_command_queue_size) ||
				!cJSON_AddStringToObject(
					document,
					"cleanup_worker",
					cleanup_worker_running ? "running" : "stopped") ||
				!cJSON_AddNumberToObject(
					document,
					"cleanup_queue",
					cleanup_queue_size) ||
				!cJSON_AddNumberToObject(
					document,
					"connections_connecting",
					connecting_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"connections_open",
					open_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"connections_reconnecting",
					reconnecting_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"connections_failed",
					failed_streams) ||
				!cJSON_AddNumberToObject(
					document,
					"transport_events",
					(double) transport_event_count) ||
				!cJSON_AddNumberToObject(
					document,
					"statistics_events",
					(double) statistics_event_count) ||
				!cJSON_AddNumberToObject(
					document,
					"error_events",
					(double) error_event_count) ||
				!cJSON_AddNumberToObject(
					document,
					"message_events",
					(double) message_event_count) ||
				!cJSON_AddNumberToObject(
					document,
					"automatic_cleanup_requests",
					(double) automatic_cleanup_request_count) ||
				!cJSON_AddNumberToObject(
					document,
					"reconnect_attempts_total",
					(double) reconnect_attempt_count) ||
				!cJSON_AddNumberToObject(
					document,
					"reconnect_successes",
					(double) reconnect_success_count) ||
				!cJSON_AddNumberToObject(
					document,
					"reconnect_exhausted",
					(double) reconnect_exhausted_count)) {
			json_status = SWITCH_STATUS_MEMERR;
		} else {
			json_status = write_json_document(stream, document);
		}
		if (document) {
			cJSON_Delete(document);
		}
		if (json_status != SWITCH_STATUS_SUCCESS) {
			stream->write_function(
					stream,
					"-ERR could not build JSON status\n");
		}
		return SWITCH_STATUS_SUCCESS;
	}

	stream->write_function(
			stream,
			"+OK module=mod_websocket_stream state=loaded "
			"build_id=%s curl=%s target_rate_hz=%d "
			"channel_to_websocket=%s websocket_to_channel=%s "
			"reconnect_attempts=%d reconnect_delay_ms=%d "
			"configured_headers=%u statistics_interval_ms=%d "
			"configured_transport_workers=%d transport_workers=%u "
			"transport_workers_running=%u transport_workers_used=%u "
			"transport_worker_max_streams=%u "
			"hard_maximum_transport_workers=%u "
			"active_streams=%u reserved_streams=%u "
			"maximum_streams=%d hard_maximum_streams=%u "
			"maximum_streams_per_channel=%d "
			"hard_maximum_streams_per_channel=%u "
			"maximum_http_headers=%d hard_maximum_http_headers=%u "
			"maximum_http_header_bytes=%d "
			"hard_maximum_http_header_bytes=%u "
			"maximum_start_variables=%d "
			"hard_maximum_start_variables=%u "
			"maximum_start_variable_bytes=%d "
			"hard_maximum_start_variable_bytes=%u "
			"maximum_start_message_bytes=%d "
			"hard_maximum_start_message_bytes=%u "
			"maximum_api_response_bytes=%d "
			"hard_maximum_api_response_bytes=%u "
			"api_commands=%s "
			"worker=%s worker_queue=%u api_worker=%s api_queue=%u "
			"cleanup_worker=%s cleanup_queue=%u "
			"connections_connecting=%u "
			"connections_open=%u connections_reconnecting=%u "
			"connections_failed=%u "
			"transport_events=%" SWITCH_UINT64_T_FMT " "
			"statistics_events=%" SWITCH_UINT64_T_FMT " "
			"error_events=%" SWITCH_UINT64_T_FMT " "
			"message_events=%" SWITCH_UINT64_T_FMT " "
			"automatic_cleanup_requests=%" SWITCH_UINT64_T_FMT " "
			"reconnect_attempts_total=%" SWITCH_UINT64_T_FMT " "
			"reconnect_successes=%" SWITCH_UINT64_T_FMT " "
			"reconnect_exhausted=%" SWITCH_UINT64_T_FMT "\n",
			MWS_BUILD_ID,
			curl_info ? curl_info->version : "unknown",
			target_sample_rate,
			channel_to_websocket ? channel_to_websocket : "unknown",
			websocket_to_channel ? websocket_to_channel : "unknown",
			reconnect_attempts,
			reconnect_delay_ms,
			configured_header_count,
			statistics_interval_ms,
			configured_transport_workers,
			transport_workers,
			transport_workers_running,
			transport_workers_used,
			transport_worker_max_streams,
			MWS_HARD_MAXIMUM_TRANSPORT_WORKERS,
			active_streams,
			reserved_streams,
			maximum_streams,
			MWS_HARD_MAXIMUM_STREAMS,
			maximum_streams_per_channel,
			MWS_HARD_MAXIMUM_STREAMS_PER_CHANNEL,
			maximum_http_headers,
			MWS_HARD_MAXIMUM_HTTP_HEADERS,
			maximum_http_header_bytes,
			MWS_HARD_MAXIMUM_HTTP_HEADER_BYTES,
			maximum_start_variables,
			MWS_HARD_MAXIMUM_START_VARIABLES,
			maximum_start_variable_bytes,
			MWS_HARD_MAXIMUM_START_VARIABLE_BYTES,
			maximum_start_message_bytes,
			MWS_HARD_MAXIMUM_START_MESSAGE_BYTES,
			maximum_api_response_bytes,
			MWS_HARD_MAXIMUM_API_RESPONSE_BYTES,
			api_commands_enabled ? "true" : "false",
			worker_running ? "running" : "stopped",
			worker_queue_size,
			api_command_worker_running ? "running" : "stopped",
			api_command_queue_size,
			cleanup_worker_running ? "running" : "stopped",
			cleanup_queue_size,
			connecting_streams,
			open_streams,
			reconnecting_streams,
			failed_streams,
			transport_event_count,
			statistics_event_count,
			error_event_count,
			message_event_count,
			automatic_cleanup_request_count,
			reconnect_attempt_count,
			reconnect_success_count,
			reconnect_exhausted_count);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_websocket_stream_load)
{
	const curl_version_info_data *curl_info =
			curl_version_info(CURLVERSION_NOW);
	CURLcode curl_result;

	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	if (!curl_info || curl_info->version_num < MWS_MINIMUM_CURL_VERSION) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"mod_websocket_stream requires libcurl >= 8.21.0 at runtime\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!(curl_info->features & CURL_VERSION_ASYNCHDNS)) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"mod_websocket_stream requires libcurl asynchronous DNS "
				"support\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(
				&globals.config_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not create configuration mutex\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(
				&globals.streams_mutex,
				SWITCH_MUTEX_UNNESTED,
				pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not create active streams mutex\n");
		return SWITCH_STATUS_FALSE;
	}

	if (load_config(SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (curl_result != CURLE_OK) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not initialize libcurl: %s\n",
				curl_easy_strerror(curl_result));
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}

	if (switch_event_reserve_subclass(MWS_TRANSPORT_EVENT_SUBCLASS) !=
			SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not reserve FreeSWITCH event subclass %s\n",
				MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}
	if (switch_event_reserve_subclass(MWS_STATISTICS_EVENT_SUBCLASS) !=
			SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not reserve FreeSWITCH event subclass %s\n",
				MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}
	if (switch_event_reserve_subclass(MWS_ERROR_EVENT_SUBCLASS) !=
			SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not reserve FreeSWITCH event subclass %s\n",
				MWS_ERROR_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}
	if (switch_event_reserve_subclass(MWS_MESSAGE_EVENT_SUBCLASS) !=
			SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not reserve FreeSWITCH event subclass %s\n",
				MWS_MESSAGE_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_ERROR_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}

	if (start_api_command_worker(pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not start FreeSWITCH API command worker\n");
		switch_event_free_subclass(MWS_MESSAGE_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_ERROR_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}

	if (start_cleanup_worker(pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not start automatic stream cleanup worker\n");
		(void) stop_api_command_worker();
		switch_event_free_subclass(MWS_MESSAGE_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_ERROR_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}

	if (start_transport_workers(pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not start WebSocket transport worker\n");
		(void) stop_cleanup_worker();
		(void) stop_api_command_worker();
		switch_event_free_subclass(MWS_MESSAGE_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_ERROR_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
		switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
		curl_global_cleanup();
		cleanup_config();
		return SWITCH_STATUS_FALSE;
	}

	*module_interface =
			switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_API(
			api_interface,
			"websocket_stream",
			"WebSocket stream module status",
			websocket_stream_api,
			MWS_API_SYNTAX);

	SWITCH_ADD_API(
			api_interface,
			"uuid_websocket_stream",
			"Control a channel WebSocket stream",
			uuid_websocket_stream_api,
			MWS_UUID_API_SYNTAX);

	SWITCH_ADD_APP(
			app_interface,
			"websocket_stream",
			"Stream channel audio over WebSocket",
			"Stream channel audio over a bidirectional WebSocket connection",
			websocket_stream_app,
			MWS_APP_SYNTAX,
			SAF_MEDIA_TAP);

	SWITCH_ADD_APP(
			app_interface,
			"websocket_stream_stop",
			"Stop a channel WebSocket stream",
			"Stop the channel's WebSocket stream without ending the call",
			websocket_stream_stop_app,
			MWS_STOP_APP_SYNTAX,
			SAF_SUPPORT_NOMEDIA);

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_NOTICE,
			"mod_websocket_stream scaffold loaded with libcurl %s\n",
			curl_info->version);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_websocket_stream_shutdown)
{
	switch_status_t api_worker_status;
	switch_status_t cleanup_worker_status;
	switch_status_t worker_status;

	api_worker_status = stop_api_command_worker();

	switch_mutex_lock(globals.transport_assignment_mutex);
	globals.transport_workers_stopping = SWITCH_TRUE;
	switch_mutex_unlock(globals.transport_assignment_mutex);

	stop_all_streams();
	cleanup_worker_status = stop_cleanup_worker();
	worker_status = stop_transport_workers();
	switch_event_free_subclass(MWS_MESSAGE_EVENT_SUBCLASS);
	switch_event_free_subclass(MWS_ERROR_EVENT_SUBCLASS);
	switch_event_free_subclass(MWS_STATISTICS_EVENT_SUBCLASS);
	switch_event_free_subclass(MWS_TRANSPORT_EVENT_SUBCLASS);
	cleanup_config();

	if (api_worker_status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not stop FreeSWITCH API command worker cleanly\n");
	}
	if (cleanup_worker_status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not stop automatic stream cleanup worker cleanly\n");
	}

	if (worker_status != SWITCH_STATUS_SUCCESS ||
			api_worker_status != SWITCH_STATUS_SUCCESS ||
			cleanup_worker_status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_CRIT,
				"Could not stop WebSocket transport worker cleanly\n");
		return SWITCH_STATUS_FALSE;
	}

	curl_global_cleanup();

	switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_NOTICE,
			"mod_websocket_stream scaffold unloaded\n");

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */

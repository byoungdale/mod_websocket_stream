# mod_websocket_stream

`mod_websocket_stream` exchanges media between a FreeSWITCH channel and a
WebSocket server.

It supports:

- Sending read, write, mixed-mono, or stereo channel audio.
- Receiving audio from the WebSocket and adding it to either side of a channel.
- Multiple independent streams on one channel.
- Automatic reconnection after unexpected connection failures.
- Per-stream status, statistics, and lifecycle events.
- Configured or per-channel HTTP request headers.

The module does not provide speech recognition, text-to-speech, or an agent
protocol. Those are handled by the WebSocket service.

## Build and install

The build requires:

- FreeSWITCH 1.11 or newer, including its development files and `freeswitch.pc`.
- CMake 3.20 or newer.
- A C11 compiler.
- pkg-config.
- libcurl 8.21 or newer.
- SpeexDSP 1.2 or newer.

```sh
pkg-config --modversion freeswitch libcurl speexdsp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
cmake --install build
```

If FreeSWITCH uses a custom prefix, add the directory containing
`freeswitch.pc` to `PKG_CONFIG_PATH`. Do the same for a libcurl installation
outside the system pkg-config search path. CMake installs the module and its
configuration into the directories reported by `freeswitch.pc`. Use
`sudo cmake --install build` when those directories are not writable by your
user.

Add the module to `autoload_configs/modules.conf.xml` if it is not already
listed:

```xml
<load module="mod_websocket_stream"/>
```

Load and verify it:

```sh
fs_cli -x "load mod_websocket_stream"
fs_cli -x "websocket_stream status"
```

After installing a rebuilt module, load the new binary with:

```sh
fs_cli -x "unload mod_websocket_stream"
fs_cli -x "load mod_websocket_stream"
```

Do not unload it while active calls are using it.

## Configure

Find the installed configuration with:

```sh
echo "$(pkg-config --variable=confdir freeswitch)/autoload_configs/websocket_stream.conf.xml"
```

A minimal configuration is:

```xml
<configuration name="websocket_stream.conf" description="WebSocket Stream">
  <settings>
    <param name="sample-rate" value="16000"/>
  </settings>
</configuration>
```

The WebSocket URL is supplied when the stream is started, not in this file.
Omitted settings use these defaults:

| Setting | Default | Meaning |
| --- | --- | --- |
| `sample-rate` | `16000` | WebSocket audio sample rate. Accepts 8000 through 48000 Hz. |
| `channel-to-websocket` | `read` | Audio FreeSWITCH sends: `read`, `write`, `mixed`, or `stereo`. |
| `websocket-to-channel` | `write` | Where received audio enters the channel: `write` or `read`. |
| `reconnect-attempts` | `5` | Retries after an unexpected connection failure. Use `0` to disable. |
| `reconnect-delay-ms` | `1000` | Initial retry delay. It doubles on each attempt, up to 30 seconds. |
| `statistics-interval-ms` | `10000` | Statistics event interval. Use `0` to disable statistics events. |
| `transport-workers` | `0` (automatic) | Transport threads. Automatic uses half the detected CPUs, capped at 8. Explicit range: 1–32. Requires a module reload. |
| `maximum-streams` | `256` | Streams allowed across FreeSWITCH. |
| `maximum-streams-per-channel` | `16` | Streams allowed on one call. |
| `maximum-http-headers` | `64` | Headers allowed on one WebSocket request. |
| `maximum-http-header-bytes` | `32768` | Total size of those headers. |
| `maximum-start-variables` | `256` | Channel variables allowed in the start message. |
| `maximum-start-variable-bytes` | `32768` | Total size of those variables. |
| `maximum-start-message-bytes` | `65536` | Size of the start message. |
| `maximum-api-response-bytes` | `65536` | Size of an API response returned over WebSocket. Minimum: 256. |
| `api-commands` | `true` | Execute plain-text WebSocket messages as FreeSWITCH API commands. JSON messages and `clear` work either way. |

For `channel-to-websocket`:

- `read` is audio arriving from the channel's remote side.
- `write` is audio FreeSWITCH is sending toward the remote side.
- `mixed` combines read and write audio into mono.
- `stereo` sends read audio on the left and write audio on the right.

For `websocket-to-channel`:

- `write` sends WebSocket audio toward the channel's remote device or service.
- `read` makes WebSocket audio look like audio arriving from that remote side,
  so a bridge, conference, or other FreeSWITCH application receives it.

Add static HTTP headers to the same `websocket_stream.conf.xml` file, after
`</settings>`:

```xml
<configuration name="websocket_stream.conf" description="WebSocket Stream">
  <settings>
    <param name="sample-rate" value="16000"/>
  </settings>
  <headers>
    <header name="Authorization" value="Bearer $${websocket_token}"/>
  </headers>
</configuration>
```

A channel variable whose name begins with `websocket_stream_header_` sets or
overrides a header for that channel:

```xml
<action application="set"
        data="websocket_stream_header_Authorization=Bearer call-token"/>
```

Reload the configuration after editing it:

```sh
fs_cli -x "websocket_stream reload"
```

The new settings apply to streams started after the reload. Existing streams
continue with the settings they started with.

## Start a stream

Start the stream from the dialplan by passing its WebSocket URL:

```xml
<extension name="websocket-test">
  <condition field="destination_number" expression="^9876$">
    <action application="set" data="playback_terminators=none"/>
    <action application="answer"/>
    <action application="websocket_stream"
            data="ws://127.0.0.1:8765/stream"/>
    <action application="playback" data="silence_stream://-1"/>
  </condition>
</extension>
```

The continuous silence supplies write-side media frames for a standalone call
receiving audio from the WebSocket. A bridged call or another application that
already produces write frames may not need it.

After a successful start, `websocket_stream_last_started_id` contains the new
stream ID. Save it before starting another stream:

```xml
<action application="websocket_stream"
        data="ws://127.0.0.1:8765/primary"/>
<action application="set"
        data="primary_stream_id=${websocket_stream_last_started_id}"/>

<action application="websocket_stream"
        data="ws://127.0.0.1:8765/secondary"/>
<action application="set"
        data="secondary_stream_id=${websocket_stream_last_started_id}"/>

<action application="websocket_stream_stop" data="${primary_stream_id}"/>
```

Use `websocket_stream_stop` with no value, or with `all`, to stop every stream
on the channel:

```xml
<action application="websocket_stream_stop" data="all"/>
```

## WebSocket messages

The first WebSocket message is JSON. It contains `stream_id`, `channel_uuid`,
`api_commands` (whether plain-text API commands are accepted on this stream),
the audio format and directions, and a snapshot of the channel variables.
Variables used as WebSocket HTTP headers are excluded from that snapshot.

- FreeSWITCH sends signed 16-bit little-endian PCM in binary WebSocket messages.
- Binary messages received from the server are mono PCM at the configured sample
  rate.
- Text beginning with `{` is a JSON message (see below). JSON messages are
  always accepted.
- Any other text is executed as a native FreeSWITCH API command when API
  commands are enabled, and its response is returned as a text message.
  Responses over the configured limit return an error. When API commands are
  disabled, the module replies with
  `-ERR FreeSWITCH API commands are disabled for this stream`.

### JSON messages

A JSON message is an object with a string `event` member of up to 128
letters, digits, or `_ - . :` characters. Messages, like API commands, are
limited to 4096 bytes.

- `{"event":"clear"}` discards WebSocket audio already queued for playback on
  this stream (barge-in). Audio sent after it is played normally. It sends no
  reply.
- Any other event is fired as a `mod_websocket_stream::message` FreeSWITCH
  event. Its body is the JSON text exactly as received, so the contract between
  the WebSocket server and the application consuming the event is entirely
  yours. Nothing is sent back on success.
- `start`, `stop`, `error`, and `mark` are reserved for the module and rejected.

```json
{"event": "transfer_requested", "target": "sales", "reason": "billing"}
```

A rejected JSON message is answered with a JSON error, for example:

```json
{"event":"error","error":"reserved_event","message":"event name is reserved by mod_websocket_stream"}
```

The `error` values are `invalid_message`, `invalid_event`, `reserved_event`,
`message_too_large`, and `event_failed`.

### Disabling API commands

Set `api-commands` to `false` to keep the WebSocket server from running
FreeSWITCH API commands. It can then only send audio, `clear`, and events, and
call control stays with whatever consumes those events, such as an ESL
application. To disable API commands for a single call, set this channel
variable before starting the stream:

```xml
<action application="set" data="websocket_stream_api_commands=false"/>
```

The channel variable can only disable API commands; it cannot enable them when
the configuration disables them.

### Sending messages to the WebSocket server

An ESL application can send a JSON message to the WebSocket server over the
stream, for example to forward a DTMF digit or announce a transfer:

```sh
uuid_websocket_stream <channel-uuid> send {"event":"dtmf","digit":"1"}
uuid_websocket_stream <channel-uuid> send <stream-id> {"event":"dtmf","digit":"1"}
```

Without a stream ID, or with `all`, the message goes to every stream on the
channel.

The message must be a UTF-8 JSON object of at most 4096 bytes; everything after
the stream ID, spaces included, is sent as one text message. The module does not
interpret it. Up to 8 messages can be queued per stream, and only while the
stream is connected. Queued messages are sent before the module's close frame,
so sending a message and then stopping the stream delivers the message first.

Together with `mod_websocket_stream::message` events this gives a two-way JSON
channel between the WebSocket server and an ESL application, while the module
only carries audio and relays messages.

## Resource limits

The defaults allow 16 streams per call and 256 streams at once. Change them in
`websocket_stream.conf.xml`. The status command shows how high each limit can
go. Header and start-variable names remain limited to 256 bytes, and their
values to 4 KiB. URLs remain limited to 4 KiB.

## Status and control

Use the FreeSWITCH API from `fs_cli` or ESL:

```sh
fs_cli -x "websocket_stream status"
fs_cli -x "websocket_stream status json"

fs_cli -x "uuid_websocket_stream <channel-uuid> status json"
fs_cli -x "uuid_websocket_stream <channel-uuid> status <stream-id> json"

fs_cli -x "uuid_websocket_stream <channel-uuid> clear"
fs_cli -x "uuid_websocket_stream <channel-uuid> clear <stream-id>"

fs_cli -x 'uuid_websocket_stream <channel-uuid> send {"event":"hello"}'
fs_cli -x 'uuid_websocket_stream <channel-uuid> send <stream-id> {"event":"hello"}'
```

The status response includes connection state, audio settings, current queue
sizes, dropped audio, playback underflows, reconnects, and playback timing.

## FreeSWITCH events

The module emits four `CUSTOM` event subclasses:

| Event subclass | When it is emitted | What it contains |
| --- | --- | --- |
| `mod_websocket_stream::transport` | A connection opens, begins reconnecting, closes, or finally fails. | Normal channel event data and variables, stream ID, state, curl result, retry information, close details, and a playback/capture statistics snapshot. |
| `mod_websocket_stream::statistics` | Periodically while a connection is open. Default: every 10 seconds. | Channel UUID, stream ID, states, audio directions, target rate, queue sizes, drops, underflows, callback timing, rate adjustment, and reconnect counters. |
| `mod_websocket_stream::error` | The playback buffer fills and the oldest audio must be discarded. | Normal channel event data and variables, WebSocket URL, stream settings, error details, queue sizes, dropped audio, timing, and curl result. |
| `mod_websocket_stream::message` | The WebSocket server sends a JSON message other than `clear`. | Normal channel event data and variables, `WebSocket-Stream-ID`, `WebSocket-Stream-Message-Event` (the message's `event` value), and the original JSON as the body. |

A `closed` transport event adds `WebSocket-Stream-Close-Initiator`: `peer` when
the WebSocket server closed the connection, or `module` when FreeSWITCH did
(stream stop or hangup). When the peer closed it, `WebSocket-Stream-Close-Code`
and `WebSocket-Stream-Close-Text` carry its close frame.

Every module header begins with `WebSocket-Stream-`, except the standard
FreeSWITCH `Unique-ID`. Common headers include:

```text
Unique-ID
WebSocket-Stream-ID
WebSocket-Stream-State
WebSocket-Stream-Playback-State
WebSocket-Stream-Target-Rate-Hz
WebSocket-Stream-Captured-Dropped-Bytes
WebSocket-Stream-Playback-Queued-Bytes
WebSocket-Stream-Playback-Peak-Queued-Bytes
WebSocket-Stream-Playback-Dropped-Samples
WebSocket-Stream-Playback-Underflow-Frames
WebSocket-Stream-Playback-Callback-Max-Lateness-Us
WebSocket-Stream-Playback-Module-Dispatch-Max-Delay-Us
WebSocket-Stream-Playback-Underflow-With-Source-Audio-Count
WebSocket-Stream-Playback-Queued-Duration-Us
WebSocket-Stream-Playback-Rate-Adjustment-Ppm
WebSocket-Stream-Reconnect-Attempts-Total
```

Transport events add curl result and retry headers. Statistics events add the
audio directions, queue capacities, playback counters and timing. Error events
add `WebSocket-Stream-Error`, `WebSocket-Stream-Error-Message`,
`WebSocket-Stream-URL`, and `WebSocket-Stream-Dropped-Samples-This-Event`.
Maximum callback lateness and the callback gap count are measured only while
playback is active. A gap is an interval arriving more than 100 ms later than
the duration of the preceding media frame. Gap and underflow observations
include their age, callback timing, queue depths, and recent WebSocket/worker
activity. A corresponding `Seen` field distinguishes "no activity yet" from an
age of zero.

To watch one subclass from an interactive `fs_cli` session:

```text
/event plain CUSTOM mod_websocket_stream::transport
```

Replace the subclass with `mod_websocket_stream::statistics`,
`mod_websocket_stream::error`, or `mod_websocket_stream::message` as needed. ESL consumers can subscribe to the
same `CUSTOM` subclasses.

## Channel variables and CDRs

When a stream ends, the module stores its terminal result and final statistics
on the channel. These identify the stream and show whether audio was dropped:

```text
websocket_stream_last_stream_id
websocket_stream_last_transport_state
websocket_stream_last_curl_code
websocket_stream_last_curl_error
websocket_stream_last_captured_drop_events
websocket_stream_last_captured_dropped_bytes
websocket_stream_last_playback_source_dropped_samples
websocket_stream_last_playback_dropped_samples
websocket_stream_last_playback_peak_queued_bytes
websocket_stream_last_playback_underflow_frames
websocket_stream_last_playback_invalid_frames
```

The playback timing summary adds these variables:

| Variable | What it tells you |
| --- | --- |
| `websocket_stream_last_playback_callback_gap_intervals` | Number of callbacks that arrived more than 100 ms late while playback was active. |
| `websocket_stream_last_playback_callback_max_lateness_us` | Worst callback lateness while playback was active. |
| `websocket_stream_last_playback_module_dispatch_max_delay_us` | Longest delay between entry into the module callback and dispatch to this stream. |
| `websocket_stream_last_playback_underflow_with_source_audio_count` | Underflows where WebSocket audio was still queued for module processing when observed. This is evidence of delay inside the playback pipeline, not proof of a specific cause. |

When a callback gap was observed, these variables describe the worst one:

| Variable | What it tells you |
| --- | --- |
| `websocket_stream_last_playback_max_gap_age_us` | How long before the stream ended the gap occurred. |
| `websocket_stream_last_playback_max_gap_lateness_us` | How late that callback was. |
| `websocket_stream_last_playback_max_gap_dispatch_delay_us` | How much of the delay occurred after the callback entered this module. |
| `websocket_stream_last_playback_max_gap_source_queued_bytes` | WebSocket audio waiting to be processed at that moment. |
| `websocket_stream_last_playback_max_gap_queued_bytes` | Playback-ready audio waiting to be mixed into the channel. |

When an underflow occurred, these variables describe the most recent one:

| Variable | What it tells you |
| --- | --- |
| `websocket_stream_last_playback_underflow_age_us` | How long before the stream ended the underflow occurred. |
| `websocket_stream_last_playback_underflow_callback_lateness_us` | Whether the FreeSWITCH media callback itself was late. |
| `websocket_stream_last_playback_underflow_dispatch_delay_us` | Time spent inside the module before this stream was processed. |
| `websocket_stream_last_playback_underflow_source_queued_bytes` | WebSocket audio still waiting for module processing. |
| `websocket_stream_last_playback_underflow_queued_bytes` | Playback-ready audio remaining when the underflow was detected. |
| `websocket_stream_last_playback_underflow_websocket_audio_age_us` | Time since the module last accepted WebSocket audio. |
| `websocket_stream_last_playback_underflow_buffer_write_age_us` | Time since the worker last produced playback-ready audio. |
| `websocket_stream_last_playback_underflow_worker_visit_age_us` | Time since the worker last entered playback processing. It does not mean processing succeeded. |

Read the values together. High callback lateness with low dispatch delay points
to timing before the module. High dispatch delay points to time inside the
module. An underflow with empty queues and old WebSocket audio usually means the
sender stopped or an utterance ended; an underflow with source audio still
queued points to delay between WebSocket input and the playback buffer. An
underflow by itself is not necessarily an error because finite audio normally
runs out.

The detailed gap variables are absent when no gap occurred, and the detailed
underflow variables are absent when no underflow occurred. These variables
describe the most recently completed stream on the channel. They are available
to hangup events and CDR modules. A CDR configuration such as a `cdr_csv`
template must explicitly include any variables it should write.

## Example server

[`example/deepgram_voice_agent`](example/deepgram_voice_agent) contains a small
bidirectional example using Deepgram speech-to-text and text-to-speech with an
OpenAI-compatible chat-completions endpoint. Its README covers setup and the
matching FreeSWITCH dialplan.

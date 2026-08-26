# Deepgram voice-agent example

This small server accepts a `mod_websocket_stream` connection, sends the
caller's mono PCM to Deepgram speech-to-text, sends each completed utterance to
an OpenAI-compatible chat-completions endpoint, and streams Deepgram
text-to-speech PCM back to the call.

It is intentionally small, but the media loop is suitable for interactive
testing. Each call has its own in-memory conversation. LLM output is streamed
into Deepgram TTS, PCM is paced toward FreeSWITCH in real time, and a completed
new caller utterance interrupts the active response instead of waiting behind
it. Raw voice-activity events are not used as interruption commands because
speaker echo and brief noise can otherwise cancel every response.

## Configure

Copy the example environment file and fill in the three required values:

```sh
cd example/deepgram_voice_agent
cp .env.example .env
$EDITOR .env
```

```dotenv
DEEPGRAM_API_KEY=your-key
LLM_BASE_URL=http://127.0.0.1:18789/v1
LLM_MODEL=
```

`LLM_BASE_URL` must expose the OpenAI-compatible
`POST /chat/completions` API. Leave `LLM_MODEL` empty when the endpoint selects
its own model. Set `LLM_API_KEY` only if that endpoint requires
Bearer authentication.

The default prompt requests plain spoken text. Common Markdown is also removed
before a response is sent to text-to-speech.

Check the configuration without starting the server:

```sh
./run.sh --check-config
```

## Run

```sh
./run.sh
```

The first run creates `.venv` and installs `aiohttp`. The server listens on:

```text
ws://127.0.0.1:8765/stream
```

Its health endpoint is available at:

```sh
curl http://127.0.0.1:8765/health
```

## FreeSWITCH settings

This minimal module configuration is enough for the example:

```xml
<configuration name="websocket_stream.conf" description="WebSocket Stream">
  <settings>
    <param name="sample-rate" value="16000"/>
  </settings>
</configuration>
```

The omitted settings use the module defaults:

- `channel-to-websocket=read` sends caller audio to the WebSocket.
- `websocket-to-channel=write` sends WebSocket audio toward the caller.
- `reconnect-attempts=5` retries unexpected connection failures five times.
- `reconnect-delay-ms=1000` starts retry delays at one second and doubles each
  subsequent delay.
- `statistics-interval-ms=10000` emits stream statistics every ten seconds.

The 16 kHz sample rate is also the default. It is shown explicitly because the
example expects mono 16 kHz PCM.

The module mixes received audio into FreeSWITCH write frames. For a standalone
voice-agent channel, continuously play silence so those frames exist:

```xml
<action application="set" data="playback_terminators=none"/>
<action application="answer"/>
<action application="websocket_stream" data="ws://127.0.0.1:8765/stream"/>
<action application="playback" data="silence_stream://-1"/>
```

The server logs each final transcript, generated response, and interruption.
On a recognized caller utterance it cancels the active LLM/TTS turn and sends
the module's raw `uuid_websocket_stream ... clear` API command to discard
queued speech. There is no example-specific command envelope.

Deepgram TTS is requested as headerless PCM. Audio is broken into 20 ms frames
and paced in real time without catch-up bursts so it does not overflow the
module's bounded playback buffer.

Use headphones while testing on one laptop. Otherwise the microphone can hear
the assistant through the speakers and treat that echo as caller barge-in.

The SIP endpoint's negotiated codec still controls what the caller hears. An
8 kHz PCMU call is narrowband even though this server and the WebSocket use
16 kHz PCM; negotiate a wideband codec such as G.722 or Opus when available.

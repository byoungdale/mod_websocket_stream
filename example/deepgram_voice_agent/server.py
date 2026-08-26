#!/usr/bin/env python3

from __future__ import annotations

import argparse
import asyncio
import contextlib
import html
import json
import logging
import os
import re
import ssl
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import certifi
from aiohttp import ClientSession, ClientTimeout, TCPConnector, WSMsgType, web


LOGGER = logging.getLogger("voice_server")
SCRIPT_DIR = Path(__file__).resolve().parent
DEEPGRAM_LISTEN_URL = "wss://api.deepgram.com/v1/listen"
DEEPGRAM_SPEAK_URL = "wss://api.deepgram.com/v1/speak"
SUPPORTED_TTS_SAMPLE_RATES = {8000, 16000, 24000, 32000, 44100, 48000}
DEFAULT_SYSTEM_PROMPT = (
    "You are a helpful voice assistant. Keep responses brief and conversational. "
    "Your response will be spoken aloud. Use plain text only. Do not use Markdown, "
    "headings, bullets, numbered lists, code blocks, URLs, brackets, or formatting "
    "symbols. Write lists as natural spoken sentences."
)
SPEECH_BREAK_RE = re.compile(r"[.!?][\"')\]}*_~`]*\s+")


def load_env_file(path: Path) -> None:
    """Load the simple KEY=value form used by this example's .env file."""
    if not path.exists():
        return

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, separator, value = line.partition("=")
        if not separator or not key.strip():
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        os.environ.setdefault(key.strip(), value)


def required_env(name: str) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        raise ValueError(f"{name} is required; copy .env.example to .env and set it")
    return value


@dataclass(frozen=True)
class Settings:
    deepgram_api_key: str
    llm_url: str
    llm_api_key: str
    llm_model: str
    system_prompt: str
    stt_model: str
    tts_model: str
    language: str
    host: str
    port: int

    @classmethod
    def from_environment(cls) -> "Settings":
        base_url = required_env("LLM_BASE_URL").rstrip("/")
        return cls(
            deepgram_api_key=required_env("DEEPGRAM_API_KEY"),
            llm_url=f"{base_url}/chat/completions",
            llm_api_key=os.getenv("LLM_API_KEY", "").strip(),
            llm_model=os.getenv("LLM_MODEL", "").strip(),
            system_prompt=os.getenv(
                "LLM_SYSTEM_PROMPT",
                DEFAULT_SYSTEM_PROMPT,
            ),
            stt_model=os.getenv("DEEPGRAM_STT_MODEL", "nova-3"),
            tts_model=os.getenv("DEEPGRAM_TTS_MODEL", "aura-2-thalia-en"),
            language=os.getenv("DEEPGRAM_LANGUAGE", "en-US"),
            host=os.getenv("VOICE_SERVER_HOST", "127.0.0.1"),
            port=int(os.getenv("VOICE_SERVER_PORT", "8765")),
        )


def deepgram_headers(settings: Settings) -> dict[str, str]:
    return {"Authorization": f"Token {settings.deepgram_api_key}"}


def deepgram_listen_url(settings: Settings, sample_rate: int) -> str:
    query = urlencode(
        {
            "model": settings.stt_model,
            "language": settings.language,
            "encoding": "linear16",
            "sample_rate": sample_rate,
            "channels": 1,
            "smart_format": "true",
            "interim_results": "true",
            "endpointing": 600,
            "utterance_end_ms": 1200,
            "vad_events": "true",
        }
    )
    return f"{DEEPGRAM_LISTEN_URL}?{query}"


def deepgram_speak_url(settings: Settings, sample_rate: int) -> str:
    query = urlencode(
        {
            "model": settings.tts_model,
            "encoding": "linear16",
            "sample_rate": sample_rate,
            "container": "none",
        }
    )
    return f"{DEEPGRAM_SPEAK_URL}?{query}"


def llm_message_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(
            item.get("text", "")
            for item in value
            if isinstance(item, dict)
        )
    return ""


def llm_response_text(data: dict[str, Any]) -> str:
    choices = data.get("choices", [])
    if not choices or not isinstance(choices[0], dict):
        return ""

    choice = choices[0]
    delta = choice.get("delta")
    if isinstance(delta, dict):
        return llm_message_text(delta.get("content"))

    message = choice.get("message")
    if isinstance(message, dict):
        return llm_message_text(message.get("content"))
    return ""


def clean_speech_text(text: str) -> str:
    """Turn common Markdown output into text suitable for speech."""
    text = html.unescape(text)
    text = re.sub(r"```.*?```", " ", text, flags=re.DOTALL)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\[[^\]]*\]", r"\1", text)
    text = re.sub(r"(?m)^\s*\[[^\]]+\]:\s+\S+.*$", " ", text)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"(?m)^\s*(?:#{1,6}|>|[-+*]|\d+[.)])\s+", "", text)
    text = re.sub(r"(?m)^\s*(?:[-*_]\s*){3,}$", " ", text)
    text = re.sub(r"(?:https?://|www\.)\S+", " ", text)
    text = text.translate(str.maketrans("", "", "*_~`[]{}"))
    text = text.replace("|", ", ")
    return re.sub(r"\s+", " ", text).strip()


async def stream_llm(
    http: ClientSession,
    settings: Settings,
    messages: list[dict[str, str]],
    on_text: Any,
) -> str:
    headers = {"Content-Type": "application/json"}
    if settings.llm_api_key:
        headers["Authorization"] = f"Bearer {settings.llm_api_key}"

    payload: dict[str, Any] = {
        "messages": messages,
        "stream": True,
    }
    if settings.llm_model:
        payload["model"] = settings.llm_model

    parts: list[str] = []
    async with http.post(
        settings.llm_url,
        headers=headers,
        json=payload,
        timeout=ClientTimeout(total=60),
    ) as response:
        if response.status >= 400:
            response_text = await response.text()
            raise RuntimeError(
                f"LLM returned HTTP {response.status}: {response_text[:500]}"
            )

        content_type = response.headers.get("Content-Type", "").lower()
        if "text/event-stream" not in content_type:
            data = json.loads(await response.text())
            text = llm_response_text(data)
            if text:
                parts.append(text)
                await on_text(text)
        else:
            async for raw_line in response.content:
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line or line.startswith(":") or not line.startswith("data:"):
                    continue

                event_data = line[5:].strip()
                if event_data == "[DONE]":
                    break

                data = json.loads(event_data)
                text = llm_response_text(data)
                if text:
                    parts.append(text)
                    await on_text(text)

    content = "".join(parts).strip()
    if not content:
        raise RuntimeError("LLM response did not contain message text")
    return content


async def stream_voice_response(
    http: ClientSession,
    settings: Settings,
    module_socket: web.WebSocketResponse,
    module_write_lock: asyncio.Lock,
    sample_rate: int,
    messages: list[dict[str, str]],
) -> str:
    url = deepgram_speak_url(settings, sample_rate)
    frame_bytes = max(2, sample_rate // 50 * 2)  # 20 ms of mono s16le PCM
    pending_audio = bytearray()
    next_send_at = asyncio.get_running_loop().time()
    flush_sent_count = 0
    flush_received_count = 0
    pending_llm_text = ""
    producer_finished = False
    playback_finished = asyncio.Event()

    async def send_frame(frame: bytes) -> None:
        nonlocal next_send_at
        loop = asyncio.get_running_loop()
        duration = len(frame) / (sample_rate * 2)
        now = loop.time()
        if next_send_at > now:
            await asyncio.sleep(next_send_at - now)
        elif now - next_send_at > duration:
            # Do not burst several frames after a long scheduler delay.
            next_send_at = now
        async with module_write_lock:
            await module_socket.send_bytes(frame)
        # Keep one continuous media clock. Starting a new 20 ms delay after
        # every send would add coroutine and socket overhead to every frame
        # and make the speech progressively slower than real time.
        next_send_at += duration

    async with http.ws_connect(
        url,
        headers=deepgram_headers(settings),
        heartbeat=20,
    ) as tts_socket:
        async def receive_audio() -> None:
            nonlocal flush_received_count
            async for message in tts_socket:
                if message.type == WSMsgType.BINARY:
                    pending_audio.extend(message.data)
                    while len(pending_audio) >= frame_bytes:
                        frame = bytes(pending_audio[:frame_bytes])
                        del pending_audio[:frame_bytes]
                        await send_frame(frame)
                    continue

                if message.type == WSMsgType.TEXT:
                    event = json.loads(message.data)
                    if event.get("type") == "Flushed":
                        if len(pending_audio) % 2:
                            raise RuntimeError("Deepgram returned an incomplete PCM sample")
                        if pending_audio:
                            await send_frame(bytes(pending_audio))
                            pending_audio.clear()
                        flush_received_count += 1
                        if producer_finished and flush_received_count >= flush_sent_count:
                            playback_finished.set()
                    elif event.get("type") == "Warning":
                        LOGGER.warning("Deepgram TTS warning: %s", event)
                    continue

                if message.type == WSMsgType.ERROR:
                    raise RuntimeError(f"Deepgram TTS failed: {tts_socket.exception()}")

            if not playback_finished.is_set():
                raise RuntimeError("Deepgram TTS closed before finishing the response")

        async def speak_text(text: str) -> None:
            nonlocal flush_sent_count
            speech_text = clean_speech_text(text)
            if not speech_text:
                return
            await tts_socket.send_json({"type": "Speak", "text": speech_text})
            await tts_socket.send_json({"type": "Flush"})
            flush_sent_count += 1

        async def send_text(text: str) -> None:
            nonlocal pending_llm_text
            pending_llm_text += text
            breaks = list(SPEECH_BREAK_RE.finditer(pending_llm_text))
            if not breaks:
                return
            break_at = breaks[-1].end()
            completed_text = pending_llm_text[:break_at]
            pending_llm_text = pending_llm_text[break_at:]
            await speak_text(completed_text)

        receiver = asyncio.create_task(receive_audio())
        playback_waiter: asyncio.Task[Any] | None = None
        try:
            reply = await stream_llm(http, settings, messages, send_text)
            await speak_text(pending_llm_text)
            pending_llm_text = ""
            producer_finished = True
            if flush_received_count >= flush_sent_count:
                playback_finished.set()

            playback_waiter = asyncio.create_task(playback_finished.wait())
            done, _ = await asyncio.wait(
                {receiver, playback_waiter},
                timeout=60,
                return_when=asyncio.FIRST_COMPLETED,
            )
            if not done:
                raise TimeoutError("Deepgram TTS did not finish within 60 seconds")
            if receiver in done:
                receiver.result()
            if not playback_finished.is_set():
                raise RuntimeError("Deepgram TTS stopped before playback completed")

            await tts_socket.send_json({"type": "Close"})
            return reply
        except asyncio.CancelledError:
            with contextlib.suppress(Exception):
                await tts_socket.send_json({"type": "Clear"})
            raise
        finally:
            receiver.cancel()
            if playback_waiter:
                playback_waiter.cancel()
            await asyncio.gather(
                receiver,
                *([playback_waiter] if playback_waiter else []),
                return_exceptions=True,
            )


async def clear_module_playback(
    module_socket: web.WebSocketResponse,
    module_write_lock: asyncio.Lock,
    channel_uuid: str,
    stream_id: str,
) -> None:
    command = f"uuid_websocket_stream {channel_uuid} clear {stream_id}"
    async with module_write_lock:
        await module_socket.send_str(command)


async def conversation_loop(
    conversation_events: asyncio.Queue[tuple[str, str]],
    http: ClientSession,
    settings: Settings,
    module_socket: web.WebSocketResponse,
    module_write_lock: asyncio.Lock,
    sample_rate: int,
    connection_name: str,
    channel_uuid: str,
    stream_id: str,
) -> None:
    messages = [{"role": "system", "content": settings.system_prompt}]
    active_turn: asyncio.Task[None] | None = None

    async def run_turn() -> None:
        try:
            reply = await stream_voice_response(
                http,
                settings,
                module_socket,
                module_write_lock,
                sample_rate,
                messages,
            )
            LOGGER.info("%s assistant: %s", connection_name, reply)
            messages.append({"role": "assistant", "content": reply})
            if len(messages) > 13:
                messages[:] = [messages[0], *messages[-12:]]
        except asyncio.CancelledError:
            LOGGER.info("%s assistant interrupted", connection_name)
            raise
        except Exception:
            LOGGER.exception("%s could not produce a voice response", connection_name)

    async def interrupt_turn() -> None:
        nonlocal active_turn
        if active_turn and not active_turn.done():
            active_turn.cancel()
            await asyncio.gather(active_turn, return_exceptions=True)
        active_turn = None
        await clear_module_playback(
            module_socket,
            module_write_lock,
            channel_uuid,
            stream_id,
        )

    try:
        while True:
            event_type, transcript = await conversation_events.get()
            if event_type != "transcript" or not transcript:
                continue

            if active_turn and not active_turn.done():
                await interrupt_turn()
            elif active_turn:
                await asyncio.gather(active_turn, return_exceptions=True)
                active_turn = None
                await clear_module_playback(
                    module_socket,
                    module_write_lock,
                    channel_uuid,
                    stream_id,
                )

            LOGGER.info("%s caller: %s", connection_name, transcript)
            messages.append({"role": "user", "content": transcript})
            active_turn = asyncio.create_task(run_turn())
    finally:
        if active_turn:
            active_turn.cancel()
            await asyncio.gather(active_turn, return_exceptions=True)


async def forward_module_audio(
    module_socket: web.WebSocketResponse,
    stt_socket: Any,
) -> None:
    async for message in module_socket:
        if message.type == WSMsgType.BINARY:
            await stt_socket.send_bytes(message.data)
        elif message.type == WSMsgType.TEXT:
            LOGGER.info("FreeSWITCH text payload: %s", message.data)
        elif message.type == WSMsgType.ERROR:
            raise RuntimeError(f"module WebSocket failed: {module_socket.exception()}")


async def receive_transcripts(
    stt_socket: Any,
    conversation_events: asyncio.Queue[tuple[str, str]],
    connection_name: str,
) -> None:
    final_parts: list[str] = []

    async for message in stt_socket:
        if message.type == WSMsgType.ERROR:
            raise RuntimeError(f"Deepgram STT failed: {stt_socket.exception()}")
        if message.type != WSMsgType.TEXT:
            continue

        event = json.loads(message.data)
        event_type = event.get("type")
        if event_type == "Results":
            alternatives = event.get("channel", {}).get("alternatives", [])
            transcript = alternatives[0].get("transcript", "") if alternatives else ""
            if event.get("is_final") and transcript.strip():
                final_parts.append(transcript.strip())
            if event.get("speech_final") and final_parts:
                await conversation_events.put(("transcript", " ".join(final_parts)))
                final_parts.clear()
        elif event_type == "UtteranceEnd" and final_parts:
            await conversation_events.put(("transcript", " ".join(final_parts)))
            final_parts.clear()
        elif event_type == "Error":
            raise RuntimeError(f"Deepgram STT error: {event}")
        elif event_type not in {"Metadata", "SpeechStarted"}:
            LOGGER.debug("%s Deepgram STT event: %s", connection_name, event)


def parse_start_message(message: Any) -> tuple[dict[str, Any], int]:
    if message.type != WSMsgType.TEXT:
        raise ValueError("the first WebSocket message must be start metadata")

    metadata = json.loads(message.data)
    audio = metadata.get("audio", {})
    if metadata.get("event") != "start" or metadata.get("version") != 1:
        raise ValueError("unsupported start metadata")
    if audio.get("format") != "s16le":
        raise ValueError("this example requires s16le audio")
    if audio.get("channels") != 1:
        raise ValueError("this example requires mono audio; use channel-to-websocket=read")

    sample_rate = audio.get("sample_rate")
    if sample_rate not in SUPPORTED_TTS_SAMPLE_RATES:
        raise ValueError(
            "Deepgram TTS requires one of these sample rates: "
            + ", ".join(str(value) for value in sorted(SUPPORTED_TTS_SAMPLE_RATES))
        )
    return metadata, sample_rate


async def handle_stream(request: web.Request, settings: Settings) -> web.WebSocketResponse:
    module_socket = web.WebSocketResponse(heartbeat=20)
    await module_socket.prepare(request)

    try:
        first_message = await asyncio.wait_for(module_socket.receive(), timeout=5)
        metadata, sample_rate = parse_start_message(first_message)
    except Exception as error:
        LOGGER.warning("rejected WebSocket connection: %s", error)
        await module_socket.close(code=1003, message=str(error).encode()[:120])
        return module_socket

    stream_id = metadata.get("stream_id", "unknown")
    channel_uuid = metadata.get("channel_uuid", "unknown")
    connection_name = f"channel={channel_uuid} stream={stream_id}"
    LOGGER.info("connected %s sample_rate=%d", connection_name, sample_rate)
    LOGGER.info(
        "%s start payload:\n%s",
        connection_name,
        json.dumps(metadata, indent=2, sort_keys=True),
    )

    tls_context = ssl.create_default_context(cafile=certifi.where())
    async with ClientSession(
        connector=TCPConnector(ssl=tls_context)
    ) as http:
        try:
            stt_socket = await http.ws_connect(
                deepgram_listen_url(settings, sample_rate),
                headers=deepgram_headers(settings),
                heartbeat=20,
            )
        except Exception:
            LOGGER.exception("%s could not connect to Deepgram STT", connection_name)
            await module_socket.close(code=1011, message=b"STT connection failed")
            return module_socket

        conversation_events: asyncio.Queue[tuple[str, str]] = asyncio.Queue()
        module_write_lock = asyncio.Lock()
        tasks = [
            asyncio.create_task(forward_module_audio(module_socket, stt_socket)),
            asyncio.create_task(
                receive_transcripts(stt_socket, conversation_events, connection_name)
            ),
            asyncio.create_task(
                conversation_loop(
                    conversation_events,
                    http,
                    settings,
                    module_socket,
                    module_write_lock,
                    sample_rate,
                    connection_name,
                    channel_uuid,
                    stream_id,
                )
            ),
        ]

        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except Exception:
            LOGGER.exception("%s audio session failed", connection_name)
        finally:
            with contextlib.suppress(Exception):
                await stt_socket.send_json({"type": "CloseStream"})
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await stt_socket.close()

    if not module_socket.closed:
        await module_socket.close()
    LOGGER.info("disconnected %s", connection_name)
    return module_socket


def create_app(settings: Settings) -> web.Application:
    async def health_handler(_: web.Request) -> web.Response:
        return web.json_response({"ok": True})

    async def stream_handler(request: web.Request) -> web.WebSocketResponse:
        return await handle_stream(request, settings)

    app = web.Application()
    app.router.add_get("/health", health_handler)
    app.router.add_get("/stream", stream_handler)
    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="PCM voice-agent example server")
    parser.add_argument(
        "--check-config",
        action="store_true",
        help="validate .env without starting the server",
    )
    arguments = parser.parse_args()

    load_env_file(SCRIPT_DIR / ".env")
    try:
        settings = Settings.from_environment()
    except (TypeError, ValueError) as error:
        raise SystemExit(f"configuration error: {error}") from error

    if arguments.check_config:
        print(f"LLM endpoint: {settings.llm_url}")
        print(f"LLM model: {settings.llm_model or '<endpoint default>'}")
        print(f"Deepgram STT model: {settings.stt_model}")
        print(f"Deepgram TTS model: {settings.tts_model}")
        print(f"Listen address: ws://{settings.host}:{settings.port}/stream")
        return

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    LOGGER.info("listening on ws://%s:%d/stream", settings.host, settings.port)
    web.run_app(create_app(settings), host=settings.host, port=settings.port)


if __name__ == "__main__":
    main()

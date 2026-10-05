"""Manually run Mac speech synthesis; no playback, cache, or persistent service.

Run ``python3 -B -m server.local_tts`` with NEMOSSI_TTS_TOKEN already set.
POST /v1/tts accepts only {"text": "..."} and returns a bounded PCM WAV.
"""

import argparse
import hmac
import io
import ipaddress
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


MAX_BODY_BYTES = 4096
MAX_HEADER_BYTES = 8192
MAX_HEADER_LINES = 40
MAX_WAV_BYTES = 960000
MAX_SECONDS = 30
SAMPLE_RATE = 16000
REQUEST_TIMEOUT = 12.0
INPUT_TIMEOUT = 2.0
MAX_SOURCE_BYTES = 4000000
TOKEN_ENV = "NEMOSSI_TTS_TOKEN"
VOICE_ENV = "NEMOSSI_TTS_VOICE"


class TTSError(Exception):
    def __init__(self, status, code):
        super().__init__(code)
        self.status = status
        self.code = code


def validate_wav(payload):
    """Validate complete RIFF PCM, remove metadata, and return canonical WAV."""
    if not isinstance(payload, bytes) or len(payload) > MAX_WAV_BYTES + 4096:
        raise TTSError(413, "audio_too_large")
    if len(payload) < 44 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise TTSError(422, "invalid_audio")
    if struct.unpack_from("<I", payload, 4)[0] != len(payload) - 8:
        raise TTSError(422, "invalid_audio")
    offset, fmt, pcm = 12, None, None
    while offset < len(payload):
        if offset + 8 > len(payload):
            raise TTSError(422, "invalid_audio")
        kind, size = struct.unpack_from("<4sI", payload, offset)
        start, end = offset + 8, offset + 8 + size
        if end + (size & 1) > len(payload):
            raise TTSError(422, "invalid_audio")
        if kind == b"fmt ":
            if fmt is not None or size not in (16, 18):
                raise TTSError(422, "invalid_audio")
            fmt = struct.unpack_from("<HHIIHH", payload, start)
            if size == 18 and payload[start + 16:end] != b"\x00\x00":
                raise TTSError(422, "invalid_audio")
        elif kind == b"data":
            if pcm is not None or fmt is None:
                raise TTSError(422, "invalid_audio")
            pcm = payload[start:end]
        offset = end + (size & 1)
    if fmt != (1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16):
        raise TTSError(422, "unsupported_audio_format")
    if not pcm or len(pcm) % 2:
        raise TTSError(422, "invalid_audio")
    if len(pcm) > MAX_SECONDS * SAMPLE_RATE * 2 or len(pcm) + 44 > MAX_WAV_BYTES:
        raise TTSError(413, "audio_too_large")
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(SAMPLE_RATE)
        audio.writeframes(pcm)
    return output.getvalue()


class MacSynthesizer:
    """Use the installed voice; text travels exclusively through stdin."""

    def __init__(self, voice="Yuna", temp_root=None):
        if not isinstance(voice, str) or not voice or len(voice) > 128 or any(ord(c) < 32 for c in voice):
            raise ValueError("An installed voice name is required")
        self.voice = voice
        self.temp_root = temp_root

    @staticmethod
    def _stop(process):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.communicate()

    def _run(self, command, deadline, cancelled, input_bytes=None, watched=(), capture=False):
        if cancelled.is_set():
            raise TTSError(503, "cancelled")
        if time.monotonic() >= deadline:
            raise TTSError(504, "synthesis_timeout")
        process = subprocess.Popen(command, stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE if capture else subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   start_new_session=True, shell=False)
        pending_input = input_bytes
        try:
            while True:
                if cancelled.is_set():
                    raise TTSError(503, "cancelled")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TTSError(504, "synthesis_timeout")
                for path, limit in watched:
                    if path.exists() and path.stat().st_size > limit:
                        raise TTSError(413, "audio_too_large")
                try:
                    output, unused = process.communicate(input=pending_input, timeout=min(0.05, remaining))
                    if process.returncode:
                        raise TTSError(502, "synthesis_failed")
                    if capture and len(output) > 65536:
                        raise TTSError(502, "synthesis_failed")
                    return output
                except subprocess.TimeoutExpired:
                    pending_input = None
        finally:
            self._stop(process)

    def __call__(self, text, deadline, cancelled):
        try:
            with tempfile.TemporaryDirectory(prefix="nemossi-tts-", dir=self.temp_root) as directory:
                catalog = self._run(["/usr/bin/say", "-v", "?"], deadline, cancelled, capture=True)
                installed = [line.split("#", 1)[0].rstrip().rsplit(None, 1)
                             for line in catalog.decode("utf-8").splitlines()]
                if [self.voice, "ko_KR"] not in installed:
                    raise TTSError(503, "korean_voice_unavailable")
                source, target = Path(directory) / "speech.aiff", Path(directory) / "speech.wav"
                self._run(["/usr/bin/say", "-v", self.voice, "-o", str(source), "--file-format=AIFF", "-f", "-"],
                          deadline, cancelled, (text + "\n").encode("utf-8"), ((source, MAX_SOURCE_BYTES),))
                if not source.is_file() or source.stat().st_size > MAX_SOURCE_BYTES:
                    raise TTSError(413, "audio_too_large")
                self._run(["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", str(source), str(target)],
                          deadline, cancelled, watched=((target, MAX_WAV_BYTES + 4096),))
                if time.monotonic() >= deadline:
                    raise TTSError(504, "synthesis_timeout")
                if cancelled.is_set():
                    raise TTSError(503, "cancelled")
                with target.open("rb") as audio:
                    payload = audio.read(MAX_WAV_BYTES + 4097)
                return validate_wav(payload)
        except (OSError, UnicodeError):
            raise TTSError(502, "synthesis_failed") from None


class _DeadlineReader:
    """Bound total read time, including clients that keep sending tiny pieces."""

    def __init__(self, connection, deadline):
        self.connection, self.deadline = connection, deadline
        self.buffer = bytearray()
        self.header_bytes = self.header_lines = 0

    def _receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TTSError(408, "request_timeout")
        self.connection.settimeout(remaining)
        try:
            data = self.connection.recv(4096)
        except (TimeoutError, socket.timeout):
            raise TTSError(408, "request_timeout") from None
        self.buffer.extend(data)
        return bool(data)

    def readline(self, size=-1):
        limit = min(size if size >= 0 else MAX_HEADER_BYTES + 1, MAX_HEADER_BYTES + 1 - self.header_bytes)
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0 or len(self.buffer) >= limit:
                count = min(newline + 1 if newline >= 0 else len(self.buffer), limit)
                break
            if not self._receive():
                count = len(self.buffer)
                break
        line = bytes(self.buffer[:count])
        del self.buffer[:count]
        self.header_bytes += count
        self.header_lines += 1
        if self.header_bytes > MAX_HEADER_BYTES or self.header_lines > MAX_HEADER_LINES:
            raise TTSError(431, "headers_too_large")
        return line

    def read(self, size):
        while len(self.buffer) < size:
            if not self._receive():
                raise TTSError(400, "incomplete_body")
        if time.monotonic() >= self.deadline:
            raise TTSError(408, "request_timeout")
        payload = bytes(self.buffer[:size])
        del self.buffer[:size]
        return payload

    def close(self):
        self.buffer.clear()


class TTSServer(ThreadingHTTPServer):
    daemon_threads = False
    allow_reuse_address = True

    def __init__(self, address, token, synthesizer, request_timeout=REQUEST_TIMEOUT, input_timeout=INPUT_TIMEOUT):
        host = address[0]
        if host == "localhost":
            host = "127.0.0.1"
        try:
            bind = ipaddress.ip_address(host)
        except ValueError:
            raise ValueError("Host must be a loopback or literal local IP address") from None
        if bind.is_unspecified or not (bind.is_private or bind.is_loopback):
            raise ValueError("Host must be a loopback or literal local IP address")
        if not isinstance(token, str) or not 16 <= len(token) <= 256 or any(not 33 <= ord(c) <= 126 for c in token):
            raise ValueError("NEMOSSI_TTS_TOKEN must contain 16–256 visible ASCII characters")
        if not 0 < input_timeout <= request_timeout <= 30:
            raise ValueError("Request timeout must be positive and at most 30 seconds")
        self.bind_host, self.token, self.synthesizer = host, token, synthesizer
        if not isinstance(address[1], int) or not 0 <= address[1] <= 65535:
            raise ValueError("Port must be between 0 and 65535")
        self.request_timeout, self.input_timeout = request_timeout, input_timeout
        self.cancelled = threading.Event()
        self.synthesis_slot = threading.BoundedSemaphore(1)
        self.request_slots = threading.BoundedSemaphore(8)
        self.clients, self.clients_lock = set(), threading.Lock()
        if bind.version == 6:
            self.address_family = socket.AF_INET6
        super().__init__((host, address[1]), TTSHandler)

    def process_request(self, request, address):
        if not self.request_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        with self.clients_lock:
            self.clients.add(request)
        try:
            super().process_request(request, address)
        except Exception:
            with self.clients_lock:
                self.clients.discard(request)
            self.request_slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            with self.clients_lock:
                self.clients.discard(request)
            self.request_slots.release()

    def server_close(self):
        self.cancelled.set()
        with self.clients_lock:
            for client in tuple(self.clients):
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        super().server_close()

    def handle_error(self, request, address):
        pass  # Never log bodies, secrets, paths, or request exceptions.


class TTSHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "NemossiLocalTTS/1"
    sys_version = ""

    def setup(self):
        super().setup()
        self.deadline = time.monotonic() + self.server.request_timeout
        self.rfile.close()
        self.rfile = _DeadlineReader(self.connection, min(self.deadline, time.monotonic() + self.server.input_timeout))

    def log_message(self, format, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except TTSError as error:
            self._error(error)
        except OSError:
            self.close_connection = True

    def handle_expect_100(self):
        self._error(TTSError(417, "expectation_failed"))
        return False

    def send_error(self, code, message=None, explain=None):
        self._error(TTSError(code, "invalid_request"))

    def _send(self, status, payload, content_type):
        self.close_connection = True
        remaining = self.deadline - time.monotonic()
        self.connection.settimeout(max(0.001, remaining))
        try:
            self.send_response(status)
            for name, value in (("Content-Type", content_type), ("Content-Length", str(len(payload))),
                                ("Connection", "close"), ("Cache-Control", "no-store"),
                                ("X-Content-Type-Options", "nosniff")):
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(payload)
        except OSError:
            pass

    def _error(self, error):
        if not hasattr(self, "requestline"):
            self.requestline, self.request_version, self.command = "", "HTTP/1.1", ""
        payload = json.dumps({"ok": False, "error": {"code": error.code}}, separators=(",", ":")).encode("ascii")
        self._send(error.status, payload, "application/json")

    def _one_header(self, name, status=400, code="invalid_headers", required=True):
        values = self.headers.get_all(name, [])
        if len(values) > 1 or (required and len(values) != 1):
            raise TTSError(status, code)
        return values[0] if values else None

    def _guard(self):
        host = self._one_header("Host", code="invalid_host")
        if any(c.isspace() or c in "/\\@,?#" for c in host):
            raise TTSError(400, "invalid_host")
        try:
            parsed = urlsplit("http://" + host)
            hosts = {self.server.bind_host}
            if ipaddress.ip_address(self.server.bind_host).is_loopback:
                hosts.update(("localhost", "127.0.0.1", "::1"))
            if parsed.hostname not in hosts or (parsed.port or 80) != self.server.server_port:
                raise ValueError
        except ValueError:
            raise TTSError(400, "invalid_host") from None
        origin = self._one_header("Origin", 403, "origin_denied", False)
        if origin is not None:
            try:
                source = urlsplit(origin)
                valid = (source.scheme == "http" and source.hostname == parsed.hostname
                         and (source.port or 80) == self.server.server_port and source.path in ("", "/")
                         and not source.query and not source.fragment and source.username is None
                         and source.password is None and not any(c.isspace() for c in origin))
            except ValueError:
                valid = False
            if not valid:
                raise TTSError(403, "origin_denied")
        site = self._one_header("Sec-Fetch-Site", 403, "origin_denied", False)
        if site is not None and site not in ("same-origin", "none"):
            raise TTSError(403, "origin_denied")
        auth = self._one_header("Authorization", 401, "unauthorized")
        try:
            valid_auth = hmac.compare_digest(auth.encode("ascii"), ("Bearer " + self.server.token).encode("ascii"))
        except UnicodeError:
            valid_auth = False
        if not valid_auth:
            raise TTSError(401, "unauthorized")
        if self.path != "/v1/tts" or self.requestline.split()[1] != "/v1/tts":
            raise TTSError(404, "not_found")

    def do_POST(self):
        acquired = False
        try:
            self._guard()
            if self.headers.get_all("Transfer-Encoding", []) or self.headers.get_all("Content-Encoding", []):
                raise TTSError(400, "unsupported_encoding")
            media = self._one_header("Content-Type", 415, "unsupported_media_type")
            if media.lower() not in ("application/json", "application/json; charset=utf-8"):
                raise TTSError(415, "unsupported_media_type")
            length = self._one_header("Content-Length", 411, "length_required")
            if not length.isascii() or not length.isdigit() or len(length) > 5:
                raise TTSError(400, "invalid_length")
            count = int(length)
            if count > MAX_BODY_BYTES:
                raise TTSError(413, "body_too_large")
            if count == 0:
                raise TTSError(400, "invalid_text")
            body = self.rfile.read(count)
            def unique(pairs):
                result = {}
                for key, value in pairs:
                    if key in result:
                        raise ValueError
                    result[key] = value
                return result
            try:
                value = json.loads(body.decode("utf-8"), object_pairs_hook=unique,
                                   parse_constant=lambda unused: (_ for _ in ()).throw(ValueError()))
                text = value["text"] if isinstance(value, dict) and set(value) == {"text"} else None
                if not isinstance(text, str) or not text.strip() or any(ord(c) < 32 and c not in "\n\t" for c in text):
                    raise ValueError
                text.encode("utf-8")
            except (ValueError, UnicodeError, RecursionError):
                raise TTSError(400, "invalid_text") from None
            acquired = self.server.synthesis_slot.acquire(blocking=False)
            if not acquired:
                raise TTSError(503, "busy")
            payload = self.server.synthesizer(text, self.deadline, self.server.cancelled)
            if time.monotonic() >= self.deadline:
                raise TTSError(504, "synthesis_timeout")
            if self.server.cancelled.is_set():
                raise TTSError(503, "cancelled")
            self._send(200, validate_wav(payload), "audio/wav")
        except TTSError as error:
            self._error(error)
        except (OSError, ValueError):
            self._error(TTSError(502, "synthesis_failed"))
        finally:
            if acquired:
                self.server.synthesis_slot.release()

    def do_GET(self):
        try:
            self._guard()
            raise TTSError(405, "method_not_allowed")
        except TTSError as error:
            self._error(error)

    do_HEAD = do_GET
    do_OPTIONS = do_GET


def create_server(host="127.0.0.1", port=8766, token=None, synthesizer=None,
                  request_timeout=REQUEST_TIMEOUT, input_timeout=INPUT_TIMEOUT):
    secret = os.environ.get(TOKEN_ENV) if token is None else token
    synth = synthesizer if synthesizer is not None else MacSynthesizer(os.environ.get(VOICE_ENV, "Yuna"))
    return TTSServer((host, port), secret, synth, request_timeout, input_timeout)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run ephemeral local Mac TTS; credentials come from the environment.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args(argv)
    try:
        server = create_server(args.host, args.port)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    try:
        print("Local TTS ready; stop with Ctrl-C. No speech or credentials are logged.", flush=True)
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

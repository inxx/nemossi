"""Bounded HTTP bridge for the first Nemossi hardware/UI demonstration."""

import argparse
import hmac
import ipaddress
import json
import os
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Optional, Tuple
from urllib.parse import unquote_to_bytes, urlsplit

from . import __version__
from .hub import MacHub
from .protocol import (
    AudioStore, RequestError, MAX_TEXT_CHARS, MAX_JSON_BYTES, MAX_AUDIO_SECONDS,
    MAX_AUDIO_BYTES, MAX_RESPONSE_CHARS, validate_wav,
)
from .transport import (
    SAMPLE_RATE, MockTransport, TransportAdapter, TransportUnavailable,
    TurnInput, UnsupportedDotTransport,
)


STATIC_FILES = {
    "index.html": "text/html; charset=utf-8",
    "style.css": "text/css; charset=utf-8",
    "app.js": "text/javascript; charset=utf-8",
    "face.js": "text/javascript; charset=utf-8",
    "audio.js": "text/javascript; charset=utf-8",
    "recorder-worklet.js": "text/javascript; charset=utf-8",
    "device.js": "text/javascript; charset=utf-8",
}


def _loopback_host(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        raise ValueError("--host must be localhost or a literal IPv4/IPv6 address")


class BridgeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: Tuple[str, int], web_root: Path, transport: TransportAdapter,
                 device_token: Optional[str], clock: Optional[Callable[[], float]] = None) -> None:
        self.bind_host = address[0]
        loopback = _loopback_host(self.bind_host)
        self.require_token = not loopback or device_token is not None
        if self.require_token and (not device_token or len(device_token) < 16):
            raise ValueError("Non-loopback access requires NEMOSSI_DEVICE_TOKEN with at least 16 characters")
        if device_token and (len(device_token) > 256 or not device_token.isascii() or any(char.isspace() for char in device_token)):
            raise ValueError("Device token must be 16–256 ASCII characters without whitespace")
        self.device_token = device_token
        self.web_root = web_root.resolve()
        self.transport = transport
        self.audio_store = AudioStore(clock=clock)
        self.hub = MacHub(transport, self.audio_store, clock=clock)
        self._request_slots = threading.BoundedSemaphore(16)
        if ":" in self.bind_host:
            self.address_family = socket.AF_INET6
        super().__init__(address, BridgeHandler)

    def process_request(self, request: socket.socket, client_address: Any) -> None:
        if not self._request_slots.acquire(blocking=False):
            response = b'{"ok":false,"error":{"code":"busy","message":"Try again shortly."}}'
            try:
                request.settimeout(1)
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + str(len(response)).encode("ascii") + b"\r\n\r\n" + response)
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._request_slots.release()
            raise

    def process_request_thread(self, request: socket.socket, client_address: Any) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._request_slots.release()

    def handle_error(self, request: socket.socket, client_address: Any) -> None:
        # No request bodies, tokens, personal paths or client addresses in logs.
        pass


class BridgeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "Nemossi/" + __version__
    sys_version = ""

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, format: str, *args: Any) -> None:
        pass

    def send_error(self, code: int, message: Optional[str] = None, explain: Optional[str] = None) -> None:
        self._error(RequestError(code, "http_error", "The HTTP request could not be processed."))

    def _send(self, status: int, payload: bytes, content_type: str,
              extra_headers: Optional[Dict[str, str]] = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Permissions-Policy", "microphone=(self), camera=(), geolocation=()")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; media-src 'self' blob:; worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'; object-src 'none'")
        for name, value in (extra_headers or {}).items():
            self.send_header(name, value)
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def _json(self, status: int, value: Dict[str, Any], extra_headers: Optional[Dict[str, str]] = None) -> None:
        self._send(status, json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8"), "application/json; charset=utf-8", extra_headers)

    def _error(self, error: RequestError) -> None:
        self.close_connection = True
        self._json(error.status, {"ok": False, "error": {"code": error.code, "message": error.message}})

    def _host(self) -> Tuple[str, int]:
        headers = self.headers.get_all("Host", [])
        if len(headers) != 1:
            raise RequestError(400, "invalid_host", "A valid Host header is required.")
        value = headers[0]
        if not value or any(char.isspace() for char in value) or any(char in value for char in "/\\@,?#"):
            raise RequestError(400, "invalid_host", "Host is not allowed.")
        try:
            parsed = urlsplit("http://" + value)
            hostname, port = parsed.hostname, parsed.port or 80
        except ValueError:
            raise RequestError(400, "invalid_host", "Host is not allowed.")
        if not hostname or port != self.server.server_port:
            raise RequestError(400, "invalid_host", "Host is not allowed.")
        if _loopback_host(self.server.bind_host):
            allowed = hostname in ("localhost", "127.0.0.1", "::1", self.server.bind_host)
        else:
            try:
                address = ipaddress.ip_address(hostname)
                bound = ipaddress.ip_address(self.server.bind_host)
                allowed = bound.is_unspecified or address == bound
            except ValueError:
                allowed = False
        if not allowed:
            raise RequestError(400, "invalid_host", "Host is not allowed.")
        return hostname, port

    def _path(self) -> str:
        # BaseHTTPRequestHandler normalizes leading //, so check the raw target too.
        raw_target = self.requestline.split()[1]
        if not self.path.startswith("/") or raw_target.startswith("//"):
            raise RequestError(400, "invalid_path", "A relative request path is required.")
        try:
            path = unquote_to_bytes(urlsplit(self.path).path).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            raise RequestError(400, "invalid_path", "Path encoding is invalid.")
        if "\\" in path or any(ord(char) < 32 for char in path) or any(part in (".", "..") for part in path.split("/")):
            raise RequestError(404, "not_found", "Resource not found.")
        return path

    def _guard(self) -> str:
        hostname, port = self._host()
        origins = self.headers.get_all("Origin", [])
        if origins:
            if len(origins) != 1:
                raise RequestError(403, "origin_denied", "Only same-origin browser requests are allowed.")
            try:
                origin = urlsplit(origins[0])
                origin_matches = (not any(char.isspace() for char in origins[0]) and origin.scheme == "http" and origin.hostname == hostname and (origin.port or 80) == port and origin.username is None and origin.password is None and origin.path in ("", "/") and not origin.query and not origin.fragment)
            except ValueError:
                origin_matches = False
            if not origin_matches:
                raise RequestError(403, "origin_denied", "Only same-origin browser requests are allowed.")
        site = self.headers.get("Sec-Fetch-Site")
        if site and site not in ("same-origin", "none"):
            raise RequestError(403, "origin_denied", "Only same-origin browser requests are allowed.")
        path = self._path()
        if path.startswith("/api/") and self.server.require_token:
            auth = self.headers.get_all("Authorization", [])
            expected = "Bearer " + self.server.device_token
            if len(auth) != 1 or not hmac.compare_digest(auth[0].encode("utf-8"), expected.encode("ascii")):
                raise RequestError(401, "unauthorized", "A valid device bearer token is required.")
        return path

    def _health(self) -> Dict[str, Any]:
        return {"ok": True, "mode": self.server.transport.name, "version": __version__,
                "audio": {"sample_rate": SAMPLE_RATE, "channels": 1, "format": "wav"},
                "transport": self.server.transport.name, "dot_connected": False,
                "dot_transport_supported": False, "dot_support_status": "unconfirmed"}

    def do_GET(self) -> None:
        try:
            path = self._guard()
            if path == "/api/health":
                self._json(200, self._health())
            elif path == "/api/hub":
                self._json(200, self.server.hub.snapshot())
            elif path == "/api/device":
                self._json(200, self.server.hub.device.snapshot())
            elif path == "/api/capabilities":
                self._json(200, {"transport": self.server.transport.name, "dot_connected": False, "dot_transport_supported": False,
                                 "dot_support_status": "unconfirmed", "real_stt": False, "real_tts": False,
                                 "audio_output": "synthetic_demo_tone", "hardware_tested": False,
                                 "max_audio_seconds": MAX_AUDIO_SECONDS, "max_text_chars": MAX_TEXT_CHARS,
                                 "audio": {"sample_rate": SAMPLE_RATE, "channels": 1, "bits_per_sample": 16, "format": "wav"}})
            elif re.fullmatch(r"/api/audio/[0-9a-f]{32}\.wav", path):
                self.server.hub.device.snapshot()  # Expire leases/deadlines before serving device audio.
                payload = self.server.audio_store.get(path[11:-4])
                if payload is None:
                    raise RequestError(404, "audio_not_found", "Audio expired or was not found.")
                self._send(200, payload, "audio/wav")
            elif path.startswith("/api/"):
                raise RequestError(404, "not_found", "Resource not found.")
            else:
                filename = "index.html" if path == "/" else path[1:]
                if filename not in STATIC_FILES:
                    raise RequestError(404, "not_found", "Resource not found.")
                target = self.server.web_root / filename
                if not target.is_file() or target.is_symlink():
                    raise RequestError(404, "not_found", "Resource not found.")
                self._send(200, target.read_bytes(), STATIC_FILES[filename])
        except RequestError as error:
            self._error(error)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            self.close_connection = True
        except Exception:
            self._error(RequestError(500, "internal_error", "The local bridge could not complete the request."))

    def _body(self, maximum: int) -> bytes:
        if self.headers.get("Transfer-Encoding") is not None:
            raise RequestError(400, "unsupported_transfer_encoding", "Send a fixed Content-Length body.")
        lengths = self.headers.get_all("Content-Length", [])
        if not lengths:
            raise RequestError(411, "length_required", "Content-Length is required.")
        if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,10}", lengths[0]):
            raise RequestError(400, "invalid_content_length", "Content-Length is invalid.")
        length = int(lengths[0])
        if length > maximum:
            raise RequestError(413, "body_too_large", "Request body exceeds the permitted size.")
        if length == 0:
            raise RequestError(400, "empty_body", "Request body is empty.")
        try:
            payload = self.rfile.read(length)
        except socket.timeout:
            raise RequestError(408, "body_timeout", "Request body was not received in time.")
        if len(payload) != length:
            raise RequestError(400, "incomplete_body", "Request body is incomplete.")
        return payload

    def _session(self, value: Any) -> Optional[str]:
        if value is None:
            return None
        if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value):
            raise RequestError(400, "invalid_session", "Session ID must be 1–64 ASCII letters, digits, underscores or hyphens.")
        return value

    def _json_input(self) -> Any:
        types = self.headers.get_all("Content-Type", [])
        if len(types) != 1 or types[0].split(";", 1)[0].strip().lower() != "application/json":
            raise RequestError(415, "unsupported_media_type", "Use application/json.")
        payload = self._body(MAX_JSON_BYTES)

        def unique_object(pairs: Any) -> Dict[str, Any]:
            value = {}
            for key, entry in pairs:
                if key in value:
                    raise ValueError("Duplicate object key")
                value[key] = entry
            return value

        def invalid_constant(value: str) -> Any:
            raise ValueError("Non-finite number")

        try:
            return json.loads(payload.decode("utf-8"), object_pairs_hook=unique_object, parse_constant=invalid_constant)
        except (ValueError, UnicodeDecodeError, RecursionError):
            raise RequestError(400, "invalid_json", "Request must contain valid UTF-8 JSON.")

    def _device_payload(self, required: set, optional: Optional[set] = None) -> Dict[str, Any]:
        value = self._json_input()
        if (not isinstance(value, dict) or not required.issubset(value)
                or not set(value).issubset(required | (optional or set()))):
            raise RequestError(400, "invalid_device_request", "Device request fields are invalid.")
        return value

    def _device_header(self, name: str, required: bool = True) -> Optional[str]:
        values = self.headers.get_all(name, [])
        if not values and not required:
            return None
        if len(values) != 1 or not re.fullmatch(r"[0-9a-f]{32}", values[0]):
            raise RequestError(400, "invalid_device_request", "A valid device connection/turn header is required.")
        return values[0]

    def _turn_input(self) -> TurnInput:
        types = self.headers.get_all("Content-Type", [])
        if len(types) != 1:
            raise RequestError(415, "unsupported_media_type", "Use application/json or audio/wav.")
        content_type = types[0].split(";", 1)[0].strip().lower()
        if content_type == "application/json":
            value = self._json_input()
            if not isinstance(value, dict) or not set(value).issubset({"text", "session_id"}):
                raise RequestError(400, "invalid_turn", "Send text and an optional session_id.")
            text_value = value.get("text")
            if not isinstance(text_value, str) or not text_value.strip() or len(text_value) > MAX_TEXT_CHARS:
                raise RequestError(400, "invalid_text", "Text must contain 1–1200 characters.")
            try:
                text_value.encode("utf-8")
            except UnicodeEncodeError:
                raise RequestError(400, "invalid_text", "Text must contain valid Unicode characters.")
            return TurnInput(text=text_value.strip(), session_id=self._session(value.get("session_id")))
        if content_type == "audio/wav":
            payload = self._body(MAX_AUDIO_BYTES)
            sessions = self.headers.get_all("X-Session-ID", [])
            if len(sessions) > 1:
                raise RequestError(400, "invalid_session", "Only one session ID is allowed.")
            session = self._session(sessions[0] if sessions else None)
            return TurnInput(audio_wav=payload, audio_duration_seconds=validate_wav(payload), session_id=session)
        raise RequestError(415, "unsupported_media_type", "Use application/json or audio/wav.")

    def do_POST(self) -> None:
        try:
            path = self._guard()
            device = self.server.hub.device
            if path == "/api/device/connect":
                self._device_payload(set())
                snapshot, created = device.connect_result()
                self._json(200, snapshot, {"X-Device-Connection-Created": "true" if created else "false"})
            elif path == "/api/device/disconnect":
                value = self._device_payload({"connection_id"})
                self._json(200, device.disconnect(value["connection_id"]))
            elif path == "/api/device/event":
                value = self._device_payload({"connection_id", "event"}, {"turn_id"})
                if not isinstance(value["event"], str) or ("turn_id" in value and value["turn_id"] is None):
                    raise RequestError(400, "invalid_device_request", "Device event fields are invalid.")
                self._json(200, device.event(value["connection_id"], value["event"], value.get("turn_id")))
            elif path == "/api/device/turn":
                connection_id = self._device_header("X-Device-Connection-ID")
                turn_id = self._device_header("X-Device-Turn-ID", required=False)
                turn = self._turn_input()
                if turn.audio_wav is not None and turn_id is None:
                    raise RequestError(400, "invalid_device_request", "Audio input requires X-Device-Turn-ID from the listen event.")
                self._json(200, self.server.hub.run_turn(turn, connection_id, turn_id))
            elif path == "/api/turn":
                self._json(200, self.server.hub.legacy_turn(self._turn_input()))
            else:
                raise RequestError(404, "not_found", "Resource not found.")
        except RequestError as error:
            self._error(error)
        except TransportUnavailable:
            self._error(RequestError(503, "dot_transport_unavailable", UnsupportedDotTransport.message))
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            self.close_connection = True
        except Exception:
            self._error(RequestError(500, "internal_error", "The local bridge could not complete the request."))

    def do_OPTIONS(self) -> None:
        self._error(RequestError(405, "method_not_allowed", "Only GET and POST are supported."))

    do_HEAD = do_OPTIONS
    do_PUT = do_OPTIONS
    do_DELETE = do_OPTIONS
    do_PATCH = do_OPTIONS


def create_server(host: str = "127.0.0.1", port: int = 8765, web_root: Optional[Path] = None,
                  transport: Optional[TransportAdapter] = None, device_token: Optional[str] = None,
                  clock: Optional[Callable[[], float]] = None) -> BridgeServer:
    root = web_root if web_root is not None else Path(__file__).resolve().parent.parent / "web"
    return BridgeServer((host, port), root, transport or MockTransport(), device_token, clock=clock)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run Nemossi's local mock hardware bridge (dot transport unconfirmed).")
    parser.add_argument("--host", default="127.0.0.1", help="Literal bind address; defaults to loopback")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--transport", choices=("mock", "dot"), default="mock")
    args = parser.parse_args()
    if args.transport == "dot":
        parser.error(UnsupportedDotTransport.message)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    try:
        loopback = _loopback_host(args.host)
        # Read only the explicitly configured device token when LAN binding needs it.
        token = None if loopback else os.environ.get("NEMOSSI_DEVICE_TOKEN")
        server = create_server(args.host, args.port, device_token=token)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print("Nemossi mock bridge listening on port {} (dot connection unconfirmed).".format(server.server_port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

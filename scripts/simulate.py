#!/usr/bin/env python3
"""Exercise Nemossi's local mock device over real HTTP, without a speaker."""

import argparse
import http.client
import io
import json
import math
import re
import socket
import sys
import threading
import time
import wave
from urllib.parse import urlsplit


VERSION = "0.2.0"
DEFAULT_URL = "http://127.0.0.1:8765"
MAX_RESPONSE_BYTES = 8192
MAX_TIMEOUT = 10.0
SCENARIOS = ("conversation", "cancel", "reconnect", "error")
STATES = frozenset(("offline", "idle", "listening", "thinking", "ready", "speaking", "error"))
IDENTIFIER = re.compile(r"^[0-9a-f]{32}$")
ERROR_CODE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")


class SimulatorError(Exception):
    """A concise error which contains no response body or transcript."""


def loopback_target(url):
    """Accept canonical local HTTP endpoints; never resolve arbitrary names."""
    message = "--url must be HTTP on 127.0.0.1, localhost, or [::1], without credentials, a path, query, or fragment."
    if not isinstance(url, str) or any(char.isspace() for char in url) or any(char in url for char in "\\?#"):
        raise SimulatorError(message)
    try:
        parsed = urlsplit(url)
        port = parsed.port if parsed.port is not None else 80
        valid = (
            parsed.scheme == "http"
            and parsed.hostname in ("127.0.0.1", "localhost", "::1")
            and re.fullmatch(r"(?:127\.0\.0\.1|localhost|\[::1\])(?::[0-9]+)?", parsed.netloc)
            and parsed.username is None
            and parsed.password is None
            and parsed.path in ("", "/")
            and not parsed.query
            and not parsed.fragment
            and "@" not in parsed.netloc
            and "%" not in parsed.netloc
            and 1 <= port <= 65535
        )
    except (ValueError, UnicodeError):
        valid = False
    if not valid:
        raise SimulatorError(message)
    # localhost is intentionally pinned to a numeric loopback address.
    return ("127.0.0.1" if parsed.hostname == "localhost" else parsed.hostname, port)


def bounded_timeout(value):
    try:
        timeout = float(value)
    except (ValueError, TypeError):
        timeout = float("nan")
    if not math.isfinite(timeout) or not 0 < timeout <= MAX_TIMEOUT:
        raise argparse.ArgumentTypeError("timeout must be greater than 0 and at most 10 seconds")
    return timeout


class BridgeClient:
    def __init__(self, url=DEFAULT_URL, timeout=3.0):
        self.host, self.port = loopback_target(url)
        try:
            self.timeout = bounded_timeout(timeout)
        except argparse.ArgumentTypeError as exc:
            raise SimulatorError(str(exc)) from None
        self.connection_created = None

    def request(self, method, path, body=None, headers=None, expected_status=200):
        headers = dict(headers or {})
        if isinstance(body, dict):
            body = json.dumps(body, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        deadline = time.monotonic() + self.timeout
        timer = None
        expired = threading.Event()
        try:
            connection.connect()
            sock = connection.sock

            def interrupt_read():
                expired.set()
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SimulatorError("bridge request timed out")
            # A socket idle timeout alone would allow an endless trickle response.
            # This deadline also bounds request headers and body reads.
            timer = threading.Timer(remaining, interrupt_read)
            timer.daemon = True
            timer.start()
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read(MAX_RESPONSE_BYTES + 1)
            self.connection_created = response.getheader("X-Device-Connection-Created")
            if expired.is_set() or time.monotonic() > deadline:
                raise SimulatorError("bridge request timed out")
            if len(payload) > MAX_RESPONSE_BYTES:
                raise SimulatorError("bridge response exceeds 8192 bytes")
            try:
                value = json.loads(payload.decode("utf-8"))
            except (ValueError, UnicodeError, RecursionError):
                raise SimulatorError("bridge response is not a JSON object") from None
            if not isinstance(value, dict):
                raise SimulatorError("bridge response is not a JSON object")
            if response.status != expected_status:
                error = value.get("error")
                code = error.get("code") if isinstance(error, dict) else None
                suffix = " ({})".format(code) if isinstance(code, str) and ERROR_CODE.fullmatch(code) else ""
                raise SimulatorError("bridge returned HTTP {}{}".format(response.status, suffix))
            return value
        except (OSError, http.client.HTTPException):
            if expired.is_set() or time.monotonic() >= deadline:
                raise SimulatorError("bridge request timed out") from None
            raise SimulatorError("could not communicate with the local bridge") from None
        finally:
            if timer is not None:
                timer.cancel()
            connection.close()


def snapshot(value):
    if not isinstance(value, dict):
        raise SimulatorError("bridge returned an invalid device snapshot")
    state = value.get("state")
    connected = value.get("connected")
    connection_id = value.get("connection_id")
    turn_id = value.get("active_turn_id")
    if (
        not isinstance(state, str)
        or state not in STATES
        or not isinstance(connected, bool)
        or connected != (state != "offline")
        or (connected and (not isinstance(connection_id, str) or not IDENTIFIER.fullmatch(connection_id)))
        or (not connected and connection_id is not None)
        or (turn_id is not None and (not isinstance(turn_id, str) or not IDENTIFIER.fullmatch(turn_id)))
        or value.get("dot_connected") is not False
        or value.get("hardware_tested") is not False
    ):
        raise SimulatorError("bridge returned an invalid or non-mock device snapshot")
    return value


def silent_demo_wav():
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(b"\x00\x00" * 2000)
    return output.getvalue()


class Simulation:
    def __init__(self, client, input_kind="text", emit=print):
        self.client = client
        self.input_kind = input_kind
        self.emit = emit
        self.owned_connection = None

    def require_state(self, value, expected, label):
        value = snapshot(value)
        if value["state"] != expected:
            raise SimulatorError("{} expected {}, received {}".format(label, expected, value["state"]))
        self.emit("{}: {}".format(label, expected))
        return value

    def connect(self):
        current = snapshot(self.client.request("GET", "/api/device"))
        if current["connected"]:
            raise SimulatorError("device already has a connection; disconnect it before running the simulator")
        value = self.client.request("POST", "/api/device/connect", {})
        if self.client.connection_created != "true":
            raise SimulatorError("connection was not created by this simulator; existing connection preserved")
        value = snapshot(value)
        self.owned_connection = value["connection_id"]
        return self.require_state(value, "idle", "connect")

    def event(self, event, expected, turn_id=None, label=None):
        body = {"connection_id": self.owned_connection, "event": event}
        if turn_id is not None:
            body["turn_id"] = turn_id
        value = self.client.request("POST", "/api/device/event", body)
        return self.require_state(value, expected, label or event)

    def turn(self):
        listening = self.event("listen", "listening")
        turn_id = listening["active_turn_id"]
        if turn_id is None:
            raise SimulatorError("listen did not assign a turn ID")
        headers = {"X-Device-Connection-ID": self.owned_connection, "X-Device-Turn-ID": turn_id}
        if self.input_kind == "wav":
            body = silent_demo_wav()
            headers["Content-Type"] = "audio/wav"
        else:
            body = {"text": "Nemossi local mock simulator."}
        reply = self.client.request("POST", "/api/device/turn", body, headers)
        ready = self.require_state(reply.get("device"), "ready", "turn ({})".format(self.input_kind))
        if reply.get("mode") != "mock" or reply.get("turn_id") != turn_id or ready["active_turn_id"] != turn_id:
            raise SimulatorError("turn response does not match this mock turn")
        return turn_id

    def disconnect(self):
        value = self.client.request("POST", "/api/device/disconnect", {"connection_id": self.owned_connection})
        self.require_state(value, "offline", "disconnect")
        self.owned_connection = None

    def cleanup(self):
        if self.owned_connection is None:
            return
        current = snapshot(self.client.request("GET", "/api/device"))
        if current["connection_id"] != self.owned_connection:
            self.owned_connection = None
            self.emit("cleanup: owned connection invalidated; current connection preserved")
            return
        if current["state"] != "idle":
            self.event("cancel", "idle", label="cleanup cancel")
        self.disconnect()

    def run(self, scenario):
        health = self.client.request("GET", "/api/health")
        capabilities = self.client.request("GET", "/api/capabilities")
        if (
            health.get("mode") != "mock"
            or health.get("dot_connected") is not False
            or health.get("dot_transport_supported") is not False
            or capabilities.get("real_stt") is not False
            or capabilities.get("real_tts") is not False
        ):
            raise SimulatorError("simulator requires the local mock bridge")
        self.emit("Nemossi simulator {} | mock only | dot=false | real STT/TTS=false".format(VERSION))
        self.emit("Playback ACKs are SIMULATED; no speaker playback or hardware test.")
        try:
            self.connect()
            if scenario == "conversation":
                turn_id = self.turn()
                self.event("playback_start", "speaking", turn_id, "playback_start (SIMULATED ACK)")
                self.event("playback_end", "idle", turn_id, "playback_end (SIMULATED ACK)")
            elif scenario == "cancel":
                self.event("listen", "listening")
                self.event("cancel", "idle")
            elif scenario == "reconnect":
                old_connection = self.owned_connection
                self.event("listen", "listening")
                self.disconnect()
                self.connect()
                if self.owned_connection == old_connection:
                    raise SimulatorError("reconnect did not replace the connection ID")
                rejected = self.client.request(
                    "POST", "/api/device/event",
                    {"connection_id": old_connection, "event": "listen"}, expected_status=409,
                )
                error = rejected.get("error")
                if not isinstance(error, dict) or error.get("code") != "connection_stale":
                    raise SimulatorError("old connection was not rejected as stale")
                self.emit("old connection: rejected (HTTP 409)")
            elif scenario == "error":
                turn_id = self.turn()
                self.event("playback_error", "error", turn_id, "playback_error (SIMULATED ACK)")
                self.event("recover", "idle")
            else:
                raise SimulatorError("unknown scenario")
        finally:
            self.cleanup()
        self.emit("scenario complete: {} (mock only)".format(scenario))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run bounded, loopback-only Nemossi mock device scenarios. No actual speaker playback.")
    parser.add_argument("--url", "--server", default=DEFAULT_URL, help="local HTTP bridge URL (default: %(default)s)")
    parser.add_argument("--scenario", choices=SCENARIOS, default="conversation", help="scripted device flow (default: %(default)s)")
    parser.add_argument("--input", choices=("text", "wav"), default="text", dest="input_kind", help="synthetic input for conversation/error; WAV is silent 16kHz mono PCM16")
    parser.add_argument("--timeout", type=bounded_timeout, default=3.0, help="absolute deadline per request in seconds, maximum 10 (default: %(default)s)")
    parser.add_argument("--version", action="version", version="Nemossi simulator " + VERSION)
    args = parser.parse_args(argv)
    try:
        Simulation(BridgeClient(args.url, args.timeout), args.input_kind).run(args.scenario)
    except SimulatorError as exc:
        print("simulation failed: {}".format(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

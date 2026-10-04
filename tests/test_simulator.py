"""CLI integration against ephemeral loopback HTTP servers; no external network."""

import argparse
import io
import subprocess
import sys
import threading
import time
import unittest
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

from scripts.simulate import BridgeClient, MAX_RESPONSE_BYTES, SimulatorError, bounded_timeout, loopback_target
from server.app import create_server
from server.transport import MockTransport


PROJECT = Path(__file__).resolve().parent.parent
SCRIPT = PROJECT / "scripts" / "simulate.py"


class RecordingMockTransport(MockTransport):
    def __init__(self):
        super().__init__()
        self.inputs = []
        self.fail = False

    def exchange(self, turn):
        self.inputs.append(turn)
        if self.fail:
            raise RuntimeError("private transport body must not be logged")
        return super().exchange(turn)


class SimulatorHTTPTests(unittest.TestCase):
    def setUp(self):
        self.transport = RecordingMockTransport()
        self.server = create_server(port=0, transport=self.transport)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:{}".format(self.server.server_port)
        self.client = BridgeClient(self.url)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def cli(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--url", self.url, "--timeout", "1", *args],
            cwd=str(PROJECT), capture_output=True, text=True, timeout=8,
        )

    def assert_offline(self, revision):
        device = self.client.request("GET", "/api/device")
        self.assertFalse(device["connected"])
        self.assertEqual(device["state"], "offline")
        self.assertIsNone(device["connection_id"])
        self.assertIsNone(device["active_turn_id"])
        self.assertEqual(device["revision"], revision)

    def test_all_four_scenarios_drive_real_http_lifecycle(self):
        cases = {
            "conversation": (["listen: listening", "turn (text): ready", "playback_start (SIMULATED ACK): speaking", "playback_end (SIMULATED ACK): idle"], 7, 1),
            "cancel": (["listen: listening", "cancel: idle"], 4, 0),
            "reconnect": (["listen: listening", "disconnect: offline", "old connection: rejected (HTTP 409)"], 5, 0),
            "error": (["turn (text): ready", "playback_error (SIMULATED ACK): error", "recover: idle"], 7, 1),
        }
        revision = 0
        turn_count = 0
        for scenario, (stages, increments, turns) in cases.items():
            with self.subTest(scenario=scenario):
                result = self.cli("--scenario", scenario)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertIn("mock only | dot=false | real STT/TTS=false", result.stdout)
                self.assertIn("Playback ACKs are SIMULATED; no speaker playback or hardware test.", result.stdout)
                for stage in stages:
                    self.assertIn(stage, result.stdout)
                self.assertIn("scenario complete: " + scenario, result.stdout)
                self.assertNotIn("Nemossi local mock simulator.", result.stdout)
                self.assertNotIn("transcript", result.stdout)
                revision += increments
                turn_count += turns
                self.assert_offline(revision)
                self.assertEqual(len(self.transport.inputs), turn_count)

    def test_synthetic_wav_has_real_upload_and_no_recording(self):
        result = self.cli("--scenario", "conversation", "--input", "wav")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("turn (wav): ready", result.stdout)
        self.assertEqual(len(self.transport.inputs), 1)
        turn = self.transport.inputs[0]
        self.assertIsNone(turn.text)
        with wave.open(io.BytesIO(turn.audio_wav), "rb") as audio:
            self.assertEqual((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()), (1, 2, 16000))
            self.assertEqual(audio.getnframes(), 2000)
            self.assertEqual(set(audio.readframes(2000)), {0})
        self.assert_offline(7)

    def test_existing_idle_and_busy_connections_are_preserved(self):
        connection = self.client.request("POST", "/api/device/connect", {})
        for busy in (False, True):
            with self.subTest(busy=busy):
                if busy:
                    self.client.request("POST", "/api/device/event", {"connection_id": connection["connection_id"], "event": "listen"})
                before = self.client.request("GET", "/api/device")
                result = self.cli("--scenario", "reconnect")
                self.assertEqual(result.returncode, 1)
                self.assertIn("device already has a connection", result.stderr)
                self.assertEqual(self.client.request("GET", "/api/device"), before)
                self.assertEqual(self.transport.inputs, [])

    def test_connect_race_never_takes_over_or_cleans_up_another_lease(self):
        lifecycle = self.server.hub.device
        original = lifecycle.connect_result
        other_connection = []

        def raced_connect():
            # A browser connects after CLI's offline GET and before its POST.
            other_connection.append(original()[0])
            return original()

        with patch.object(lifecycle, "connect_result", side_effect=raced_connect):
            result = self.cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("existing connection preserved", result.stderr)
        self.assertEqual(len(other_connection), 1)
        self.assertEqual(self.client.request("GET", "/api/device"), other_connection[0])
        self.assertEqual(self.transport.inputs, [])

    def test_transport_failure_cleans_up_only_its_own_connection(self):
        self.transport.fail = True
        result = self.cli()
        self.assertEqual(result.returncode, 1)
        self.assertIn("bridge returned HTTP 500", result.stderr)
        self.assertIn("cleanup cancel: idle", result.stdout)
        self.assertIn("disconnect: offline", result.stdout)
        self.assertNotIn("private transport body", result.stdout + result.stderr)
        self.assert_offline(6)

    def test_help_version_and_server_alias(self):
        for flag, expected in (("--help", "--scenario"), ("--version", "Nemossi simulator 0.2.0")):
            result = self.cli(flag)
            self.assertEqual(result.returncode, 0)
            self.assertIn(expected, result.stdout)
        result = self.cli("--server", self.url, "--scenario", "cancel")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_offline(4)


class URLAndDeadlineTests(unittest.TestCase):
    def test_only_canonical_loopback_http_urls_are_accepted(self):
        for url, target in (
            ("http://127.0.0.1:8765", ("127.0.0.1", 8765)),
            ("http://localhost:1234/", ("127.0.0.1", 1234)),
            ("http://[::1]:4321", ("::1", 4321)),
            ("http://127.0.0.1", ("127.0.0.1", 80)),
        ):
            with self.subTest(url=url):
                self.assertEqual(loopback_target(url), target)
        invalid = (
            "https://127.0.0.1", "http://192.168.1.2", "http://0.0.0.0", "http://example.invalid",
            "http://localhost.example.invalid", "http://127.1", "http://2130706433", "http://0177.0.0.1",
            "http://127.0.0.1:0", "http://127.0.0.1:65536", "http://127.0.0.1:",
            "http://user:secret@127.0.0.1", "http://127.0.0.1@external.invalid",
            "http://127.0.0.1/private", "http://127.0.0.1?token=secret", "http://127.0.0.1#fragment",
            "http://127.0.0.1?", "http://127.0.0.1#", " http://127.0.0.1", "http://127.0.0.1\n",
            "http://localhost%2e", "http://[::1", "http://127.0.0.1\\external.invalid", "file:///tmp/example",
        )
        for url in invalid:
            with self.subTest(url=url):
                with self.assertRaises(SimulatorError):
                    loopback_target(url)

    def test_timeout_values_are_finite_positive_and_bounded(self):
        self.assertEqual(bounded_timeout("0.1"), 0.1)
        self.assertEqual(bounded_timeout("10"), 10)
        for value in ("0", "-1", "11", "nan", "inf", "invalid"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                bounded_timeout(value)

    def run_bad_server(self, behavior, expected):
        stopped = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if behavior == "stall":
                    stopped.wait(2)
                    return
                payload = b"private malformed body" if behavior == "invalid" else b'{"padding":"' + b"x" * MAX_RESPONSE_BYTES + b'"}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    if behavior == "trickle":
                        self.wfile.write(b"{")
                        self.wfile.flush()
                        while not stopped.wait(0.02):
                            self.wfile.write(b" ")
                            self.wfile.flush()
                    else:
                        self.wfile.write(payload)
                except OSError:
                    pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        thread.start()
        started = time.monotonic()
        try:
            client = BridgeClient("http://127.0.0.1:{}".format(server.server_port), timeout=0.15)
            with self.assertRaisesRegex(SimulatorError, expected) as failure:
                client.request("GET", "/api/health")
            self.assertNotIn("private malformed body", str(failure.exception))
            self.assertLess(time.monotonic() - started, 1)
        finally:
            stopped.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_invalid_and_oversized_responses_are_bounded_and_not_logged(self):
        for behavior, expected in (("invalid", "not a JSON object"), ("oversized", "exceeds 8192 bytes")):
            with self.subTest(behavior=behavior):
                self.run_bad_server(behavior, expected)

    def test_deadline_bounds_stalled_headers_and_continuous_trickle(self):
        for behavior in ("stall", "trickle"):
            with self.subTest(behavior=behavior):
                self.run_bad_server(behavior, "timed out")


if __name__ == "__main__":
    unittest.main()

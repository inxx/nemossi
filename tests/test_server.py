"""Real HTTP integration checks, using only the Python standard library."""

import http.client
import io
import json
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

from server.app import (
    AudioStore, MAX_AUDIO_BYTES, MAX_JSON_BYTES, MAX_TEXT_CHARS, STATIC_FILES,
    create_server,
)
from server.transport import TransportUnavailable, TurnInput, UnsupportedDotTransport


def wav_bytes(seconds=0.125, channels=1, rate=16000, width=2):
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(channels)
        audio.setsampwidth(width)
        audio.setframerate(rate)
        audio.writeframes(b"\x00" * int(rate * seconds) * channels * width)
    return output.getvalue()


class BridgeHTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        for filename in STATIC_FILES:
            (cls.root / filename).write_text("fixture " + filename, encoding="utf-8")
        (cls.root / "private.json").write_text("must stay private", encoding="utf-8")
        cls.server = create_server(port=0, web_root=cls.root)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.port = cls.server.server_port

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.directory.cleanup()

    def request(self, method="GET", path="/api/health", body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def assert_error(self, result, status, code):
        actual_status, headers, payload = result
        self.assertEqual(actual_status, status)
        self.assertIn("application/json", headers["Content-Type"])
        value = json.loads(payload)
        self.assertFalse(value["ok"])
        self.assertEqual(value["error"]["code"], code)
        self.assertNotIn("Access-Control-Allow-Origin", headers)

    def post_json(self, value, headers=None):
        all_headers = {"Content-Type": "application/json"}
        all_headers.update(headers or {})
        return self.request("POST", "/api/turn", json.dumps(value, ensure_ascii=False).encode("utf-8"), all_headers)

    def test_health_and_capabilities_never_claim_dot_connection(self):
        status, headers, payload = self.request()
        self.assertEqual(status, 200)
        health = json.loads(payload)
        self.assertTrue(health["ok"])
        self.assertEqual(health["mode"], "mock")
        self.assertEqual(health["version"], "0.2.0")
        self.assertEqual(health["audio"], {"sample_rate": 16000, "channels": 1, "format": "wav"})
        self.assertFalse(health["dot_connected"])
        self.assertFalse(health["dot_transport_supported"])
        self.assertEqual(health["dot_support_status"], "unconfirmed")
        self.assertEqual(headers["Cache-Control"], "no-store")
        capabilities = json.loads(self.request(path="/api/capabilities")[2])
        self.assertFalse(capabilities["real_stt"])
        self.assertFalse(capabilities["real_tts"])
        self.assertFalse(capabilities["hardware_tested"])
        self.assertEqual(capabilities["audio_output"], "synthetic_demo_tone")

    def test_text_turn_is_local_mock_and_downloadable_wav(self):
        status, _, payload = self.post_json({"text": "안녕 네모씨", "session_id": "test-session_01"})
        self.assertEqual(status, 200)
        turn = json.loads(payload)
        self.assertEqual(turn["transcript"], "안녕 네모씨")
        self.assertEqual(turn["mode"], "mock")
        self.assertIn("데모", turn["text"])
        self.assertIn("dot", turn["text"])
        self.assertIsInstance(turn["duration_ms"], int)
        self.assertGreaterEqual(turn["duration_ms"], 0)
        self.assertRegex(turn["turn_id"], r"^[0-9a-f]{32}$")
        self.assertRegex(turn["audio"]["url"], r"^/api/audio/[0-9a-f]{32}\.wav$")
        self.assertEqual(turn["audio"]["sample_rate"], 16000)
        status, headers, audio_payload = self.request(path=turn["audio"]["url"])
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "audio/wav")
        with wave.open(io.BytesIO(audio_payload), "rb") as audio:
            self.assertEqual((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()), (1, 2, 16000))
            self.assertAlmostEqual(audio.getnframes() / audio.getframerate(), 0.48)
            self.assertNotEqual(set(audio.readframes(audio.getnframes())), {0})

    def test_wav_upload_uses_honest_placeholder_transcript(self):
        status, _, payload = self.request("POST", "/api/turn", wav_bytes(), {"Content-Type": "audio/wav", "X-Session-ID": "voice_session"})
        self.assertEqual(status, 200)
        turn = json.loads(payload)
        self.assertIn("데모 음성 입력", turn["transcript"])
        self.assertIn("실제 음성 인식 아님", turn["transcript"])
        self.assertIn("0.12초", turn["transcript"])
        self.assertEqual(self.request(path=turn["audio"]["url"])[0], 200)

    def test_audio_format_and_duration_boundaries(self):
        for configuration in ({"channels": 2}, {"rate": 8000}, {"width": 1}):
            with self.subTest(configuration=configuration):
                self.assert_error(self.request("POST", "/api/turn", wav_bytes(**configuration), {"Content-Type": "audio/wav"}), 422, "unsupported_audio_format")
        self.assertEqual(self.request("POST", "/api/turn", wav_bytes(seconds=15), {"Content-Type": "audio/wav"})[0], 200)
        self.assert_error(self.request("POST", "/api/turn", wav_bytes(seconds=15.01), {"Content-Type": "audio/wav"}), 413, "audio_too_long")

    def test_malformed_wav_rejected_before_transport(self):
        good = wav_bytes()
        for bad in (b"bad audio", good[:-1], good + b"extra", wav_bytes(seconds=0)):
            with self.subTest(length=len(bad)):
                self.assert_error(self.request("POST", "/api/turn", bad, {"Content-Type": "audio/wav"}), 400, "invalid_wav")
        bad_rate = bytearray(good)
        struct.pack_into("<I", bad_rate, 28, 1)
        self.assert_error(self.request("POST", "/api/turn", bytes(bad_rate), {"Content-Type": "audio/wav"}), 422, "unsupported_audio_format")

    def test_json_schema_and_encoding_errors_are_deterministic(self):
        for value in ({}, {"text": " "}, {"text": 42}, {"text": "a", "extra": 1}, {"text": "x" * (MAX_TEXT_CHARS + 1)}, ["x"]):
            with self.subTest(value_type=type(value).__name__):
                self.assert_error(self.post_json(value), 400, "invalid_turn" if isinstance(value, list) or "extra" in value else "invalid_text")
        for body in (b"{", b"\xff", b'{"text":"a","text":"b"}', b'{"text":NaN}', b"[" * 2000):
            with self.subTest(length=len(body)):
                self.assert_error(self.request("POST", "/api/turn", body, {"Content-Type": "application/json"}), 400, "invalid_json")
        self.assert_error(self.request("POST", "/api/turn", b'{"text":"\\ud800"}', {"Content-Type": "application/json"}), 400, "invalid_text")
        self.assertEqual(self.post_json({"text": "a" * MAX_TEXT_CHARS})[0], 200)

    def test_session_ids_are_bounded_and_ascii(self):
        for value in ("", "../private", "한글", "a" * 65, 123):
            with self.subTest(value=value):
                self.assert_error(self.post_json({"text": "hello", "session_id": value}), 400, "invalid_session")
        self.assert_error(self.request("POST", "/api/turn", wav_bytes(), {"Content-Type": "audio/wav", "X-Session-ID": "bad/session"}), 400, "invalid_session")

    def test_body_size_and_media_type_limits(self):
        # The server must reject oversized declarations before waiting for a body.
        self.assert_error(self.request("POST", "/api/turn", b"", {"Content-Type": "application/json", "Content-Length": str(MAX_JSON_BYTES + 1)}), 413, "body_too_large")
        self.assert_error(self.request("POST", "/api/turn", b"", {"Content-Type": "audio/wav", "Content-Length": str(MAX_AUDIO_BYTES + 1)}), 413, "body_too_large")
        self.assert_error(self.request("POST", "/api/turn", b"x", {"Content-Type": "audio/webm"}), 415, "unsupported_media_type")
        self.assert_error(self.request("POST", "/api/turn", b"", {"Content-Type": "application/json"}), 400, "empty_body")
        self.assert_error(self.request("POST", "/api/turn", b"{}", {"Content-Type": "application/json", "Transfer-Encoding": "chunked"}), 400, "unsupported_transfer_encoding")

    def test_static_allowlist_and_traversal_protection(self):
        self.assertEqual(self.request(path="/")[0], 200)
        for filename in STATIC_FILES:
            self.assertEqual(self.request(path="/" + filename)[0], 200)
        for path in ("/private.json", "/../private.json", "/%2e%2e/private.json", "/%2e%2e%2fprivate.json", "/..%5cprivate.json", "/server/app.py", "//index.html"):
            with self.subTest(path=path):
                self.assertIn(self.request(path=path)[0], (400, 404))
        target = self.root / "app.js"
        original = target.read_bytes()
        try:
            target.unlink()
            target.symlink_to(self.root / "private.json")
            self.assert_error(self.request(path="/app.js"), 404, "not_found")
        finally:
            target.unlink()
            target.write_bytes(original)

    def test_browser_origin_host_and_fetch_metadata_protection(self):
        same = "http://127.0.0.1:{}".format(self.port)
        self.assertEqual(self.post_json({"text": "allowed"}, {"Origin": same, "Sec-Fetch-Site": "same-origin"})[0], 200)
        for origin in ("https://example.invalid", "null", "http://localhost:{}".format(self.port), same + "/extra", "http://@127.0.0.1:{}".format(self.port), same + " "):
            with self.subTest(origin=origin):
                self.assert_error(self.post_json({"text": "blocked"}, {"Origin": origin}), 403, "origin_denied")
        self.assert_error(self.request(headers={"Sec-Fetch-Site": "cross-site"}), 403, "origin_denied")
        self.assert_error(self.request(headers={"Host": "attacker.invalid:{}".format(self.port)}), 400, "invalid_host")
        self.assert_error(self.request(headers={"Host": "localhost:1"}), 400, "invalid_host")
        self.assert_error(self.request(headers={"Host": "localhost@127.0.0.1:{}".format(self.port)}), 400, "invalid_host")

    def test_absent_expired_audio_and_unknown_routes(self):
        self.assert_error(self.request(path="/api/audio/" + "0" * 32 + ".wav"), 404, "audio_not_found")
        self.assert_error(self.request(path="/api/unknown"), 404, "not_found")
        self.assert_error(self.request("POST", "/api/unknown", b"{}"), 404, "not_found")
        self.assert_error(self.request("OPTIONS", "/api/turn"), 405, "method_not_allowed")

    def raw_request(self, headers, body=b""):
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
            connection.sendall(headers.encode("ascii") + body)
            connection.shutdown(socket.SHUT_WR)
            chunks = []
            while True:
                block = connection.recv(8192)
                if not block:
                    break
                chunks.append(block)
        raw = b"".join(chunks)
        head, payload = raw.split(b"\r\n\r\n", 1)
        return int(head.split(b" ", 2)[1]), json.loads(payload)

    def test_request_framing_errors(self):
        prefix = "POST /api/turn HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nContent-Type: application/json\r\n".format(self.port)
        for extra, body, status, code in (("", b"{}", 411, "length_required"),
                                          ("Content-Length: -1\r\n", b"", 400, "invalid_content_length"),
                                          ("Content-Length: 2\r\nContent-Length: 2\r\n", b"{}", 400, "invalid_content_length"),
                                          ("Content-Length: 20\r\n", b"{}", 400, "incomplete_body")):
            with self.subTest(code=code):
                actual, payload = self.raw_request(prefix + extra + "\r\n", body)
                self.assertEqual(actual, status)
                self.assertEqual(payload["error"]["code"], code)


class TokenHTTPTests(unittest.TestCase):
    def test_lan_binding_requires_token_before_opening_socket(self):
        for token in (None, "short", "spaces are not allowed", "nonascii한글123456789"):
            with self.subTest(token_kind="unset" if token is None else "invalid"):
                with self.assertRaises(ValueError):
                    create_server(host="0.0.0.0", port=0, device_token=token)
        with self.assertRaises(ValueError):
            create_server(host="external.invalid", port=0)

    def test_bearer_token_enforced_for_all_api_routes(self):
        token = "integration-fixture-token-only"
        server = create_server(port=0, device_token=token)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for supplied, expected in ((None, 401), ("Bearer wrong", 401), ("Bearer " + token, 200)):
                for path in ("/api/health", "/api/capabilities"):
                    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
                    try:
                        headers = {} if supplied is None else {"Authorization": supplied}
                        connection.request("GET", path, headers=headers)
                        response = connection.getresponse()
                        self.assertEqual(response.status, expected)
                        response.read()
                    finally:
                        connection.close()
            for supplied, expected in ((None, 401), ("Bearer " + token, 200)):
                connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
                try:
                    headers = {"Content-Type": "application/json"}
                    if supplied:
                        headers["Authorization"] = supplied
                    connection.request("POST", "/api/turn", body=b'{"text":"token test"}', headers=headers)
                    response = connection.getresponse()
                    self.assertEqual(response.status, expected)
                    value = json.loads(response.read())
                    if expected == 200:
                        audio_url = value["audio"]["url"]
                finally:
                    connection.close()
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            try:
                connection.request("GET", audio_url)
                response = connection.getresponse()
                self.assertEqual(response.status, 401)
                response.read()
            finally:
                connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


class StoreAndTransportTests(unittest.TestCase):
    def test_audio_store_has_count_and_expiry_bounds(self):
        store = AudioStore(ttl=10, capacity=2)
        with patch("server.app.time.monotonic", return_value=100):
            first = store.put(b"first")
            second = store.put(b"second")
            third = store.put(b"third")
            self.assertIsNone(store.get(first))
            self.assertEqual(store.get(second), b"second")
            self.assertEqual(store.get(third), b"third")
        with patch("server.app.time.monotonic", return_value=110):
            self.assertIsNone(store.get(second))
            self.assertIsNone(store.get(third))

    def test_unsupported_dot_transport_and_cli_fail_explicitly(self):
        with self.assertRaisesRegex(TransportUnavailable, "not been confirmed"):
            UnsupportedDotTransport().exchange(TurnInput(text="hello"))
        project = Path(__file__).resolve().parent.parent
        result = subprocess.run([sys.executable, "-m", "server", "--transport", "dot"], cwd=str(project), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=3)
        self.assertEqual(result.returncode, 2)
        self.assertIn("dot voice transport is unavailable", result.stderr)
        self.assertNotIn("listening", result.stdout)


if __name__ == "__main__":
    unittest.main()

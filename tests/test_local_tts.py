"""Local TTS checks: real HTTP and processes, with no playback or credentials."""

import http.client
import io
import json
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

from server.local_tts import (
    MAX_BODY_BYTES, MAX_HEADER_BYTES, MAX_SOURCE_BYTES, MAX_WAV_BYTES,
    MacSynthesizer, TTSError, create_server, validate_wav,
)


TOKEN = "local-test-placeholder-secret"


def wav_bytes(frames=160, rate=16000, channels=1, width=2):
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as audio:
        audio.setnchannels(channels)
        audio.setsampwidth(width)
        audio.setframerate(rate)
        audio.writeframes(b"\x00" * frames * channels * width)
    return buffer.getvalue()


class LocalTTSTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        def synth(text, deadline, cancelled):
            self.calls.append(text)
            return wav_bytes()
        self.server = create_server(port=0, token=TOKEN, synthesizer=synth)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()
        self.port = self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())

    def request(self, value=None, body=None, method="POST", path="/v1/tts", headers=None):
        if body is None:
            body = json.dumps(value if value is not None else {"text": "테스트입니다."}, ensure_ascii=False).encode("utf-8")
        request_headers = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
        request_headers.update(headers or {})
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body, request_headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def raw(self, headers, body=b""):
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
            request = ("POST /v1/tts HTTP/1.1\r\n" + headers + "\r\n\r\n").encode("ascii") + body
            connection.sendall(request)
            response = http.client.HTTPResponse(connection)
            response.begin()
            return response.status, dict(response.getheaders()), response.read()

    def assert_error(self, result, status, code):
        actual, headers, body = result
        self.assertEqual(actual, status)
        self.assertEqual(json.loads(body), {"ok": False, "error": {"code": code}})
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertNotIn("Access-Control-Allow-Origin", headers)

    def test_real_http_returns_canonical_wav_and_never_caches_or_logs_text(self):
        with patch("sys.stderr", new_callable=io.StringIO) as stderr:
            for unused in range(2):
                status, headers, payload = self.request({"text": "안녕하세요. 테스트입니다."})
                self.assertEqual(status, 200)
                self.assertEqual(headers["Content-Type"], "audio/wav")
                self.assertEqual(headers["Cache-Control"], "no-store")
                self.assertEqual(payload, wav_bytes())
                self.assertLessEqual(len(payload), MAX_WAV_BYTES)
            self.assertEqual(stderr.getvalue(), "")
        self.assertEqual(self.calls, ["안녕하세요. 테스트입니다."] * 2)

    def test_authentication_is_required_and_duplicates_are_rejected(self):
        for auth in ("", "Bearer wrong-placeholder", "Basic " + TOKEN):
            with self.subTest(auth=auth):
                self.assert_error(self.request(headers={"Authorization": auth}), 401, "unauthorized")
        common = "Host: 127.0.0.1:{}\r\nContent-Type: application/json\r\nContent-Length: 2".format(self.port)
        self.assert_error(self.raw(common, b"{}"), 401, "unauthorized")
        self.assert_error(self.raw(common + "\r\nAuthorization: Bearer " + TOKEN + "\r\nAuthorization: Bearer " + TOKEN, b"{}"), 401, "unauthorized")
        self.assertEqual(self.calls, [])

    def test_host_origin_and_fetch_site_guards_precede_synthesis(self):
        for host in ("evil.example:{}".format(self.port), "127.0.0.1:1", "user@127.0.0.1:{}".format(self.port)):
            self.assert_error(self.request(headers={"Host": host}), 400, "invalid_host")
        for origin in ("https://127.0.0.1:{}".format(self.port), "http://evil.example", "null"):
            self.assert_error(self.request(headers={"Origin": origin}), 403, "origin_denied")
        self.assert_error(self.request(headers={"Sec-Fetch-Site": "cross-site"}), 403, "origin_denied")
        self.assertEqual(self.calls, [])
        self.assertEqual(self.request(headers={"Origin": "http://127.0.0.1:{}".format(self.port), "Sec-Fetch-Site": "same-origin"})[0], 200)

    def test_json_schema_encoding_and_body_limits(self):
        for value in ({}, {"text": ""}, {"text": " "}, {"text": 12}, {"text": "x", "voice": "Yuna"}, ["x"], {"text": "\x00"}):
            self.assert_error(self.request(value), 400, "invalid_text")
        for body in (b"{", b"\xff", b'{"text":"a","text":"b"}', b'{"text":NaN}', b'{"text":"\\ud800"}', b"[" * 2000):
            self.assert_error(self.request(body=body), 400, "invalid_text")
        self.assert_error(self.request(body=b"", headers={"Content-Length": str(MAX_BODY_BYTES + 1)}), 413, "body_too_large")
        self.assert_error(self.request(body=b"{}", headers={"Content-Type": "text/plain"}), 415, "unsupported_media_type")
        self.assert_error(self.request(body=b"{}", headers={"Transfer-Encoding": "chunked"}), 400, "unsupported_encoding")
        self.assertEqual(self.calls, [])
        text = "x" * (MAX_BODY_BYTES - len(b'{"text":""}'))
        self.assertEqual(self.request(body=b'{"text":"' + text.encode() + b'"}')[0], 200)

    def test_http_header_bounds_duplicates_and_exact_request_target(self):
        self.assert_error(self.request(headers={"X-Extra": "a" * MAX_HEADER_BYTES}), 431, "headers_too_large")
        common = "Host: 127.0.0.1:{0}\r\nHost: localhost:{0}\r\nAuthorization: Bearer {1}\r\nContent-Type: application/json\r\nContent-Length: 2".format(self.port, TOKEN)
        self.assert_error(self.raw(common, b"{}"), 400, "invalid_host")
        self.assert_error(self.request(method="GET"), 405, "method_not_allowed")
        for path in ("http://127.0.0.1:{}/v1/tts".format(self.port), "/v1/tts?url=https://example.com", "//v1/tts"):
            self.assert_error(self.request(path=path), 404, "not_found")
        self.assertEqual(self.calls, [])

    def test_slow_body_has_an_absolute_deadline(self):
        self.server.input_timeout = 0.15
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as connection:
            request = "POST /v1/tts HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nAuthorization: Bearer {}\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{{".format(self.port, TOKEN)
            start = time.monotonic()
            connection.sendall(request.encode("ascii"))
            response = http.client.HTTPResponse(connection)
            response.begin()
            result = response.status, dict(response.getheaders()), response.read()
            self.assert_error(result, 408, "request_timeout")
            self.assertLess(time.monotonic() - start, 0.8)
        self.assertEqual(self.calls, [])

    def test_dripping_headers_cannot_extend_deadline(self):
        self.server.input_timeout = 0.15
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as connection:
            connection.sendall(b"POST /v1/tts HTTP/1.1\r\nX-Slow: ")
            def drip():
                for unused in range(8):
                    time.sleep(0.04)
                    try:
                        connection.sendall(b"a")
                    except OSError:
                        break
            writer = threading.Thread(target=drip)
            start = time.monotonic()
            writer.start()
            response = http.client.HTTPResponse(connection)
            response.begin()
            self.assertEqual(response.status, 408)
            response.read()
            self.assertLess(time.monotonic() - start, 0.8)
            writer.join(1)
        self.assertEqual(self.calls, [])

    def test_concurrent_synthesis_is_rejected_without_queue(self):
        entered, release = threading.Event(), threading.Event()
        def synth(text, deadline, cancelled):
            entered.set()
            release.wait(1)
            return wav_bytes()
        self.server.synthesizer = synth
        first = []
        worker = threading.Thread(target=lambda: first.append(self.request()))
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            self.assert_error(self.request(), 503, "busy")
        finally:
            release.set()
            worker.join(2)
        self.assertEqual(first[0][0], 200)

    def test_output_format_failure_releases_synthesis_slot(self):
        for bad in (b"bad", wav_bytes(rate=8000), wav_bytes(channels=2), wav_bytes(width=1), wav_bytes(frames=480000), wav_bytes()[:-1]):
            self.server.synthesizer = lambda *args, output=bad: output
            result = self.request()
            self.assertIn(result[0], (413, 422))
        self.server.synthesizer = lambda *args: wav_bytes()
        self.assertEqual(self.request()[0], 200)

    def test_http_synthesis_timeout_kills_the_process_and_cleans_temporary_audio(self):
        with tempfile.TemporaryDirectory() as root:
            synth = MacSynthesizer(temp_root=root)
            real_run = synth._run
            processes = []
            original = subprocess.Popen
            def spawn(*args, **kwargs):
                process = original(*args, **kwargs)
                processes.append(process)
                return process
            def stalled(command, deadline, cancelled, input_bytes=None, watched=(), capture=False):
                if capture:
                    return b"Yuna ko_KR # neutral voice catalog\n"
                for path, unused in watched:
                    path.write_bytes(b"temporary fixture")
                return real_run([sys.executable, "-c", "import time; time.sleep(10)"], deadline, cancelled, watched=watched)
            synth._run = stalled
            self.server.synthesizer = synth
            self.server.request_timeout, self.server.input_timeout = 0.2, 0.1
            with patch("server.local_tts.subprocess.Popen", side_effect=spawn):
                start = time.monotonic()
                self.assert_error(self.request(), 504, "synthesis_timeout")
                self.assertLess(time.monotonic() - start, 1)
            self.assertTrue(processes)
            self.assertTrue(all(process.poll() is not None for process in processes))
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_server_close_cancels_active_http_synthesis_and_removes_audio(self):
        with tempfile.TemporaryDirectory() as root:
            synth, entered, processes = MacSynthesizer(temp_root=root), threading.Event(), []
            real_run, original = synth._run, subprocess.Popen
            def spawn(*args, **kwargs):
                process = original(*args, **kwargs)
                processes.append(process)
                entered.set()
                return process
            def stalled(command, deadline, cancelled, input_bytes=None, watched=(), capture=False):
                if capture:
                    return b"Yuna ko_KR # neutral voice catalog\n"
                for path, unused in watched:
                    path.write_bytes(b"temporary fixture")
                return real_run([sys.executable, "-c", "import time; time.sleep(10)"], deadline, cancelled, watched=watched)
            synth._run = stalled
            self.server.synthesizer = synth
            result = []
            def request():
                try:
                    result.append(self.request())
                except (OSError, http.client.HTTPException):
                    result.append("closed")
            worker = threading.Thread(target=request)
            with patch("server.local_tts.subprocess.Popen", side_effect=spawn):
                worker.start()
                self.assertTrue(entered.wait(1))
                start = time.monotonic()
                self.server.shutdown()
                self.server.server_close()
                worker.join(1)
                self.assertLess(time.monotonic() - start, 1)
            self.assertFalse(worker.is_alive())
            self.assertEqual(result, ["closed"])
            self.assertTrue(all(process.poll() is not None for process in processes))
            self.assertEqual(list(Path(root).iterdir()), [])


class SynthesisResourceTests(unittest.TestCase):
    def test_secret_and_binding_validation(self):
        for secret in (None, "short", "x" * 257, "placeholder secret", "비밀" * 16):
            with patch.dict("os.environ", {}, clear=True):
                with self.assertRaises(ValueError):
                    create_server(port=0, token=secret)
        for host in ("0.0.0.0", "example.com", "8.8.8.8"):
            with self.assertRaises(ValueError):
                create_server(host=host, port=0, token=TOKEN)

    def test_stdin_only_and_cleanup_on_success_failure_and_cancellation(self):
        for outcome in ("success", "failure", "cancel"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as root:
                synth, cancelled, commands = MacSynthesizer(temp_root=root), threading.Event(), []
                text = "테스트입니다. $(false); --not-an-option"
                def runner(command, deadline, event, input_bytes=None, watched=(), capture=False):
                    if capture:
                        return b"Yuna ko_KR # neutral voice catalog\n"
                    commands.append((command, input_bytes))
                    for path, unused in watched:
                        path.write_bytes(wav_bytes() if path.suffix == ".wav" else b"fixture")
                    if outcome == "failure":
                        raise TTSError(502, "synthesis_failed")
                    if outcome == "cancel":
                        event.set()
                synth._run = runner
                if outcome == "success":
                    self.assertEqual(synth(text, time.monotonic() + 1, cancelled), wav_bytes())
                    self.assertEqual(commands[0][1], (text + "\n").encode("utf-8"))
                    self.assertNotIn(text, commands[0][0])
                    self.assertEqual(commands[0][0][0], "/usr/bin/say")
                    self.assertEqual(commands[1][0][0], "/usr/bin/afconvert")
                else:
                    with self.assertRaises(TTSError):
                        synth(text, time.monotonic() + 1, cancelled)
                self.assertEqual(list(Path(root).iterdir()), [])

    def test_uninstalled_or_non_korean_voice_is_rejected_without_synthesis(self):
        with tempfile.TemporaryDirectory() as root:
            for voice in ("Unavailable", "Samantha"):
                synth = MacSynthesizer(voice=voice, temp_root=root)
                with patch.object(synth, "_run", return_value=b"Yuna ko_KR # voice\nSamantha en_US # voice\n") as run:
                    with self.assertRaises(TTSError) as error:
                        synth("테스트입니다.", time.monotonic() + 1, threading.Event())
                    self.assertEqual(error.exception.code, "korean_voice_unavailable")
                    self.assertEqual(run.call_count, 1)
                self.assertEqual(list(Path(root).iterdir()), [])

    def test_subprocess_cancel_and_file_growth_kill_child(self):
        for outcome in ("cancel", "large"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as root:
                synth, cancelled, processes = MacSynthesizer(), threading.Event(), []
                original = subprocess.Popen
                def spawn(*args, **kwargs):
                    process = original(*args, **kwargs)
                    processes.append(process)
                    return process
                output = Path(root) / "temporary.aiff"
                if outcome == "large":
                    output.write_bytes(b"x" * (MAX_SOURCE_BYTES + 1))
                timer = threading.Timer(0.08, cancelled.set) if outcome == "cancel" else None
                if timer:
                    timer.start()
                try:
                    with patch("server.local_tts.subprocess.Popen", side_effect=spawn), self.assertRaises(TTSError) as error:
                        synth._run([sys.executable, "-c", "import time; time.sleep(10)"], time.monotonic() + 1,
                                   cancelled, watched=((output, MAX_SOURCE_BYTES),))
                    self.assertEqual(error.exception.code, "cancelled" if outcome == "cancel" else "audio_too_large")
                    self.assertTrue(all(process.poll() is not None for process in processes))
                finally:
                    if timer:
                        timer.cancel()

    def test_riff_size_format_integrity_and_metadata_removal(self):
        original = wav_bytes()
        metadata = b"JUNK" + struct.pack("<I", 4) + b"none"
        decorated = original[:12] + metadata + original[12:]
        decorated = decorated[:4] + struct.pack("<I", len(decorated) - 8) + decorated[8:]
        self.assertEqual(validate_wav(decorated), original)
        for offset, value in ((28, 1), (32, 4), (34, 8)):
            damaged = bytearray(original)
            struct.pack_into("<I" if offset == 28 else "<H", damaged, offset, value)
            with self.assertRaises(TTSError):
                validate_wav(bytes(damaged))
        for damaged in (original + b"extra", original[:-2], wav_bytes(frames=0)):
            with self.assertRaises(TTSError):
                validate_wav(damaged)


if __name__ == "__main__":
    unittest.main()

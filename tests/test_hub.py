"""Device lifecycle and real HTTP checks for the Mac mini hub boundary."""

import http.client
import io
import json
import threading
import unittest
import wave

from server.app import AudioStore, create_server
from server.device import DeviceLifecycle
from server.hub import MacHub
from server.protocol import RequestError
from server.transport import MockTransport, TransportUnavailable, TurnInput, UnsupportedDotTransport


SNAPSHOT_FIELDS = {
    "connected", "connection_id", "revision", "state", "active_turn_id",
    "last_event", "last_error", "device_kind", "hardware_tested", "dot_connected",
}


class FakeClock:
    def __init__(self):
        self.value = 1000.0
        self.lock = threading.Lock()

    def __call__(self):
        with self.lock:
            return self.value

    def advance(self, seconds):
        with self.lock:
            self.value += seconds


def wav_bytes():
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(b"\x00" * 4000)
    return output.getvalue()


class DelayedTransport(MockTransport):
    """Hold a real HTTP turn while a separate request changes device ownership."""

    def __init__(self):
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()
        self.last_input = None

    def exchange(self, turn):
        self.last_input = turn
        self.entered.set()
        if not self.release.wait(timeout=5):
            raise RuntimeError("Test did not release the delayed transport")
        return super().exchange(turn)


class DeviceLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.device = DeviceLifecycle(clock=self.clock)

    def assert_failure(self, callback, code):
        with self.assertRaises(RequestError) as caught:
            callback()
        self.assertEqual(caught.exception.status, 409)
        self.assertEqual(caught.exception.code, code)

    def connected(self):
        return self.device.connect()["connection_id"]

    def publish(self, ticket):
        return self.device.complete_turn(ticket, lambda: "audio-fixture")

    def test_initial_snapshot_and_idempotent_connection(self):
        self.assertEqual(self.device.snapshot(), {
            "connected": False, "connection_id": None, "revision": 0,
            "state": "offline", "active_turn_id": None, "last_event": "initial",
            "last_error": None, "device_kind": "nemossi_simulator",
            "hardware_tested": False, "dot_connected": False,
        })
        first = self.device.connect()
        self.assertEqual(set(first), SNAPSHOT_FIELDS)
        self.assertTrue(first["connected"])
        self.assertEqual(first["state"], "idle")
        self.assertRegex(first["connection_id"], r"^[0-9a-f]{32}$")
        self.assertEqual(self.device.connect(), first)

    def test_listen_turn_and_playback_require_acknowledgements(self):
        connection_id = self.connected()
        listening = self.device.event(connection_id, "listen")
        self.assertEqual(listening["state"], "listening")
        turn_id = listening["active_turn_id"]
        self.assertRegex(turn_id, r"^[0-9a-f]{32}$")
        ticket = self.device.begin_turn(connection_id, turn_id, audio_input=True)
        self.assertEqual(ticket.turn_id, turn_id)
        self.assertEqual(self.device.snapshot()["state"], "thinking")
        audio_id, ready = self.publish(ticket)
        self.assertEqual(audio_id, "audio-fixture")
        self.assertEqual(ready["state"], "ready")
        self.assertEqual(ready["active_turn_id"], turn_id)
        speaking = self.device.event(connection_id, "playback_start", turn_id)
        self.assertEqual(speaking["state"], "speaking")
        idle = self.device.event(connection_id, "playback_end", turn_id)
        self.assertEqual(idle["state"], "idle")
        self.assertIsNone(idle["active_turn_id"])
        self.assertLess(listening["revision"], ready["revision"])
        self.assertLess(ready["revision"], speaking["revision"])
        self.assertLess(speaking["revision"], idle["revision"])

    def test_stale_connection_turn_and_invalid_transition_preserve_state(self):
        old_connection = self.connected()
        self.device.disconnect(old_connection)
        connection_id = self.connected()
        self.assertNotEqual(connection_id, old_connection)
        initial = self.device.snapshot()
        self.assert_failure(lambda: self.device.event(old_connection, "listen"), "connection_stale")
        self.assert_failure(lambda: self.device.event(connection_id, "recover"), "invalid_transition")
        self.assertEqual(self.device.snapshot(), initial)
        listening = self.device.event(connection_id, "listen")
        self.assert_failure(lambda: self.device.begin_turn(connection_id, "0" * 32, audio_input=True), "turn_stale")
        self.assertEqual(self.device.snapshot(), listening)

    def test_cancelled_completion_never_calls_audio_publication(self):
        connection_id = self.connected()
        ticket = self.device.begin_turn(connection_id)
        self.device.event(connection_id, "cancel")
        calls = []
        self.assert_failure(lambda: self.device.complete_turn(ticket, lambda: calls.append("published")), "turn_invalidated")
        self.assertEqual(calls, [])
        self.assertFalse(self.device.fail_turn(ticket, "transport_error"))
        self.assertEqual(self.device.snapshot()["state"], "idle")
        self.assertIsNone(self.device.snapshot()["last_error"])

    def test_disconnection_and_reconnection_invalidate_previous_ticket(self):
        old_connection = self.connected()
        ticket = self.device.begin_turn(old_connection)
        self.device.disconnect(old_connection)
        current = self.device.connect()
        calls = []
        self.assert_failure(lambda: self.device.complete_turn(ticket, lambda: calls.append("published")), "turn_invalidated")
        self.assertFalse(self.device.fail_turn(ticket, "old_error"))
        self.assertEqual(calls, [])
        self.assertEqual(self.device.snapshot(), current)

    def test_heartbeat_renews_connection_lease(self):
        connection_id = self.connected()
        self.clock.advance(59)
        self.assertTrue(self.device.snapshot()["connected"])
        self.device.event(connection_id, "heartbeat")
        self.clock.advance(59)
        self.assertTrue(self.device.snapshot()["connected"])
        self.clock.advance(1)
        expired = self.device.snapshot()
        self.assertFalse(expired["connected"])
        self.assertEqual(expired["state"], "offline")
        self.assertEqual(expired["last_error"], "lease_expired")
        self.assertIsNone(expired["connection_id"])
        self.assert_failure(lambda: self.device.event(connection_id, "heartbeat"), "connection_stale")

    def test_listening_deadline_expires_without_a_background_thread(self):
        connection_id = self.connected()
        self.device.event(connection_id, "listen")
        self.clock.advance(19.999)
        self.assertEqual(self.device.snapshot()["state"], "listening")
        self.device.event(connection_id, "heartbeat")
        self.clock.advance(0.001)
        expired = self.device.snapshot()
        self.assertEqual(expired["state"], "error")
        self.assertIsNone(expired["active_turn_id"])
        self.assertEqual(expired["last_error"], "listening_timeout")
        self.assertEqual(self.device.event(connection_id, "recover")["state"], "idle")

    def test_processing_deadlines_invalidate_audio_and_turn(self):
        for state in ("thinking", "ready", "speaking"):
            with self.subTest(state=state):
                clock = FakeClock()
                device = DeviceLifecycle(clock=clock)
                connection_id = device.connect()["connection_id"]
                ticket = device.begin_turn(connection_id)
                if state != "thinking":
                    device.complete_turn(ticket, lambda: "audio-fixture")
                if state == "speaking":
                    device.event(connection_id, "playback_start", ticket.turn_id)
                clock.advance(19.999)
                self.assertEqual(device.snapshot()["state"], state)
                clock.advance(0.001)
                expired = device.snapshot()
                self.assertEqual(expired["state"], "error")
                self.assertEqual(expired["last_error"], state + "_timeout")
                self.assertIsNone(expired["active_turn_id"])
                calls = []
                self.assert_failure(lambda: device.complete_turn(ticket, lambda: calls.append("published")), "turn_invalidated")
                self.assertEqual(calls, [])


class HubHTTPTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.server = create_server(port=0, clock=self.clock)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method="GET", path="/api/device", value=None, headers=None, body=None, include_headers=False):
        all_headers = dict(headers or {})
        if value is not None:
            body = json.dumps(value, ensure_ascii=False).encode("utf-8")
            all_headers.setdefault("Content-Type", "application/json")
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=all_headers)
            response = connection.getresponse()
            payload = response.read()
            content_type = response.getheader("Content-Type", "")
            value = json.loads(payload) if "application/json" in content_type else payload
            if include_headers:
                return response.status, value, dict(response.getheaders())
            return response.status, value
        finally:
            connection.close()

    def connect(self):
        status, snapshot = self.request("POST", "/api/device/connect", {})
        self.assertEqual(status, 200)
        self.assertEqual(set(snapshot), SNAPSHOT_FIELDS)
        return snapshot

    def event(self, connection_id, event, turn_id=None):
        value = {"connection_id": connection_id, "event": event}
        if turn_id is not None:
            value["turn_id"] = turn_id
        return self.request("POST", "/api/device/event", value)

    def turn(self, connection_id=None, turn_id=None, text="안녕 네모씨", audio=False):
        headers = {}
        if connection_id is not None:
            headers["X-Device-Connection-ID"] = connection_id
        if turn_id is not None:
            headers["X-Device-Turn-ID"] = turn_id
        if audio:
            headers["Content-Type"] = "audio/wav"
            return self.request("POST", "/api/device/turn", headers=headers, body=wav_bytes())
        return self.request("POST", "/api/device/turn", {"text": text}, headers=headers)

    def assert_error(self, result, status, code):
        actual_status, payload = result
        self.assertEqual(actual_status, status, payload)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["error"]["code"], code)

    def test_hub_contract_and_future_peer_never_claim_dot_or_hardware(self):
        status, snapshot = self.request()
        self.assertEqual(status, 200)
        status, hub = self.request(path="/api/hub")
        self.assertEqual(status, 200)
        self.assertEqual(hub, {
            "hub": {"role": "mac_mini", "mode": "mock"}, "device": snapshot,
            "dot_connected": False, "dot_support_status": "unconfirmed",
            "future_peer": {"status": "reserved", "via": "external_relay", "direct_mac_link_assumed": False},
        })

    def test_connect_text_reply_and_playback_acknowledgements(self):
        connected = self.connect()
        self.assertEqual(self.connect(), connected)
        connection_id = connected["connection_id"]
        status, reply = self.turn(connection_id)
        self.assertEqual(status, 200)
        self.assertEqual(reply["mode"], "mock")
        self.assertEqual(reply["device"]["state"], "ready")
        self.assertEqual(reply["device"]["connection_id"], connection_id)
        self.assertEqual(reply["device"]["active_turn_id"], reply["turn_id"])
        self.assertFalse(reply["device"]["dot_connected"])
        self.assertEqual(self.request(path=reply["audio"]["url"])[0], 200)
        self.assertEqual(self.event(connection_id, "playback_start", reply["turn_id"])[1]["state"], "speaking")
        self.assertEqual(self.request()[1]["state"], "speaking")
        self.assertEqual(self.event(connection_id, "playback_end", reply["turn_id"])[1]["state"], "idle")

    def test_simultaneous_connects_create_only_one_connection(self):
        barrier = threading.Barrier(6)
        results = []
        failures = []

        def connect_together():
            try:
                barrier.wait(timeout=2)
                results.append(self.request("POST", "/api/device/connect", {}, include_headers=True))
            except Exception as error:
                failures.append(error)

        workers = [threading.Thread(target=connect_together, daemon=True) for _ in range(6)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(timeout=3)
        self.assertTrue(all(not worker.is_alive() for worker in workers))
        self.assertEqual(failures, [])
        self.assertEqual(len(results), 6)
        self.assertEqual({status for status, _, _ in results}, {200})
        self.assertEqual(len({value["connection_id"] for _, value, _ in results}), 1)
        self.assertEqual({value["revision"] for _, value, _ in results}, {1})
        creation = [headers["X-Device-Connection-Created"] for _, _, headers in results]
        self.assertEqual(creation.count("true"), 1)
        self.assertEqual(creation.count("false"), 5)

    def test_audio_turn_uses_listen_ticket_and_rejects_stale_ticket(self):
        connection_id = self.connect()["connection_id"]
        status, listening = self.event(connection_id, "listen")
        self.assertEqual(status, 200)
        turn_id = listening["active_turn_id"]
        self.assert_error(self.turn(connection_id, audio=True), 400, "invalid_device_request")
        self.assert_error(self.turn(connection_id, "0" * 32, audio=True), 409, "turn_stale")
        self.assertEqual(self.request()[1], listening)
        status, reply = self.turn(connection_id, turn_id, audio=True)
        self.assertEqual(status, 200)
        self.assertEqual(reply["turn_id"], turn_id)
        self.assertIn("실제 음성 인식 아님", reply["transcript"])
        self.assertEqual(reply["device"]["state"], "ready")

    def test_invalid_connection_and_transition_are_conflicts(self):
        self.assert_error(self.turn(), 400, "invalid_device_request")
        connection_id = self.connect()["connection_id"]
        self.assert_error(self.event(connection_id, "recover"), 409, "invalid_transition")
        self.assertEqual(self.request()[1]["state"], "idle")
        self.assertEqual(self.request("POST", "/api/device/disconnect", {"connection_id": connection_id})[1]["state"], "offline")
        new_connection = self.connect()["connection_id"]
        self.assertNotEqual(connection_id, new_connection)
        self.assert_error(self.turn(connection_id), 409, "connection_stale")
        self.assertEqual(self.request()[1]["connection_id"], new_connection)

    def test_malformed_device_messages_are_rejected_before_state_change(self):
        initial = self.connect()
        connection_id = initial["connection_id"]
        for path, value in (
            ("connect", {"extra": True}), ("connect", []),
            ("disconnect", {}), ("disconnect", {"connection_id": 1}),
            ("event", {"connection_id": connection_id}),
            ("event", {"connection_id": connection_id, "event": "unknown"}),
            ("event", {"connection_id": connection_id, "event": "listen", "extra": True}),
        ):
            with self.subTest(path=path, value=value):
                self.assert_error(self.request("POST", "/api/device/" + path, value), 400, "invalid_device_request")
        for body in (b"{", b'{"connection_id":"a","connection_id":"b"}', b'{"event":NaN}', b"\xff"):
            with self.subTest(body=body):
                self.assert_error(self.request("POST", "/api/device/event", body=body, headers={"Content-Type": "application/json"}), 400, "invalid_json")
        self.assertEqual(self.request()[1], initial)

    def test_cancel_revokes_ready_audio_immediately(self):
        connection_id = self.connect()["connection_id"]
        status, reply = self.turn(connection_id)
        self.assertEqual(status, 200)
        self.assertEqual(self.request(path=reply["audio"]["url"])[0], 200)
        status, snapshot = self.event(connection_id, "cancel")
        self.assertEqual(status, 200)
        self.assertEqual(snapshot["state"], "idle")
        self.assertIsNone(snapshot["active_turn_id"])
        self.assert_error(self.request(path=reply["audio"]["url"]), 404, "audio_not_found")
        self.assert_error(self.event(connection_id, "playback_start", reply["turn_id"]), 409, "turn_stale")

    def test_disconnect_and_expiry_revoke_ready_audio_on_download(self):
        for action in ("disconnect", "deadline", "lease"):
            with self.subTest(action=action):
                current = self.request()[1]
                if current["connected"]:
                    self.request("POST", "/api/device/disconnect", {"connection_id": current["connection_id"]})
                connection_id = self.connect()["connection_id"]
                status, reply = self.turn(connection_id)
                self.assertEqual(status, 200)
                if action == "disconnect":
                    self.request("POST", "/api/device/disconnect", {"connection_id": connection_id})
                else:
                    self.clock.advance(20 if action == "deadline" else 60)
                # Audio access itself must expire device ownership, with no prior poll.
                self.assert_error(self.request(path=reply["audio"]["url"]), 404, "audio_not_found")
                self.assertEqual(self.request()[1]["state"], "error" if action == "deadline" else "offline")

    def test_playback_error_revokes_audio_and_recovery_allows_next_turn(self):
        connection_id = self.connect()["connection_id"]
        status, reply = self.turn(connection_id)
        self.assertEqual(status, 200)
        status, failed = self.event(connection_id, "playback_error", reply["turn_id"])
        self.assertEqual(status, 200)
        self.assertEqual(failed["state"], "error")
        self.assertEqual(failed["last_error"], "playback_failed")
        self.assertIsNone(failed["active_turn_id"])
        self.assert_error(self.request(path=reply["audio"]["url"]), 404, "audio_not_found")
        status, recovered = self.event(connection_id, "recover")
        self.assertEqual(status, 200)
        self.assertEqual(recovered["state"], "idle")
        self.assertIsNone(recovered["last_error"])
        status, next_reply = self.turn(connection_id, text="다시 시도")
        self.assertEqual(status, 200)
        self.assertNotEqual(reply["turn_id"], next_reply["turn_id"])
        self.assertEqual(next_reply["device"]["state"], "ready")

    def test_new_hub_routes_require_token_and_same_origin(self):
        # A test-only token enables the same guard used for LAN binding.
        token = "hub-integration-fixture-only"
        self.server.device_token = token
        self.server.require_token = True
        for path in ("/api/hub", "/api/device"):
            self.assert_error(self.request(path=path), 401, "unauthorized")
            self.assertEqual(self.request(path=path, headers={"Authorization": "Bearer " + token})[0], 200)
        for path, value in (
            ("connect", {}), ("disconnect", {"connection_id": "0" * 32}),
            ("event", {"connection_id": "0" * 32, "event": "listen"}), ("turn", {"text": "hello"}),
        ):
            with self.subTest(path=path):
                self.assert_error(self.request("POST", "/api/device/" + path, value), 401, "unauthorized")
                headers = {"Authorization": "Bearer " + token, "Origin": "https://example.invalid"}
                self.assert_error(self.request("POST", "/api/device/" + path, value, headers), 403, "origin_denied")
        self.assertFalse(self.request(headers={"Authorization": "Bearer " + token})[1]["connected"])
        same_origin = "http://127.0.0.1:{}".format(self.server.server_port)
        headers = {"Authorization": "Bearer " + token, "Origin": same_origin}
        self.assertEqual(self.request("POST", "/api/device/connect", {}, headers)[0], 200)

    def test_legacy_turn_remains_independent_of_device_connection(self):
        before = self.connect()
        status, reply = self.request("POST", "/api/turn", {"text": "legacy caller", "session_id": "legacy-session"})
        self.assertEqual(status, 200)
        self.assertEqual(reply["transcript"], "legacy caller")
        self.assertNotIn("device", reply)
        self.assertEqual(self.request()[1], before)

    def test_lazy_state_timeout_and_lease_expiry_visible_via_http(self):
        connection_id = self.connect()["connection_id"]
        self.event(connection_id, "listen")
        self.clock.advance(20)
        timed_out = self.request()[1]
        self.assertEqual(timed_out["state"], "error")
        self.assertIsNone(timed_out["active_turn_id"])
        self.assertEqual(self.event(connection_id, "recover")[1]["state"], "idle")
        self.clock.advance(60)
        self.assertEqual(self.request(path="/api/hub")[1]["device"]["state"], "offline")
        self.assert_error(self.event(connection_id, "heartbeat"), 409, "connection_stale")

    def test_blocked_turn_is_invalidated_by_cancel_disconnect_reconnect_and_timeout(self):
        for action in ("cancel", "disconnect", "reconnect", "timeout"):
            with self.subTest(action=action):
                # The previous case ends idle or offline; use a fresh connection.
                current = self.request()[1]
                if current["connected"]:
                    self.request("POST", "/api/device/disconnect", {"connection_id": current["connection_id"]})
                connection_id = self.connect()["connection_id"]
                transport = DelayedTransport()
                self.server.transport = transport
                self.server.hub.voice_transport = transport
                result = []
                failures = []

                def submit():
                    try:
                        result.append(self.turn(connection_id))
                    except Exception as error:
                        failures.append(error)

                worker = threading.Thread(target=submit, daemon=True)
                worker.start()
                try:
                    self.assertTrue(transport.entered.wait(timeout=2), "HTTP request did not enter transport")
                    self.assertEqual(self.request()[1]["state"], "thinking")
                    if action == "cancel":
                        expected = self.event(connection_id, "cancel")[1]
                    elif action in ("disconnect", "reconnect"):
                        expected = self.request("POST", "/api/device/disconnect", {"connection_id": connection_id})[1]
                        if action == "reconnect":
                            expected = self.connect()
                    else:
                        self.clock.advance(20)
                        expected = self.request()[1]
                    before_publication = len(self.server.audio_store._items)
                    transport.release.set()
                    worker.join(timeout=3)
                    self.assertFalse(worker.is_alive(), "Invalidated HTTP turn did not finish")
                    self.assertEqual(failures, [])
                    self.assertEqual(len(result), 1)
                    self.assert_error(result[0], 409, "turn_invalidated")
                    self.assertNotIn("audio", result[0][1])
                    self.assertEqual(len(self.server.audio_store._items), before_publication)
                    self.assertEqual(self.request()[1], expected)
                finally:
                    transport.release.set()
                    worker.join(timeout=3)

    def test_idempotent_connect_during_thinking_preserves_pending_turn(self):
        connection_id = self.connect()["connection_id"]
        transport = DelayedTransport()
        self.server.transport = transport
        self.server.hub.voice_transport = transport
        result = []
        failures = []

        def submit():
            try:
                result.append(self.turn(connection_id))
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=submit, daemon=True)
        worker.start()
        try:
            self.assertTrue(transport.entered.wait(timeout=2))
            thinking = self.request()[1]
            self.assertEqual(thinking["state"], "thinking")
            status, repeated, headers = self.request("POST", "/api/device/connect", {}, include_headers=True)
            self.assertEqual(status, 200)
            self.assertEqual(repeated, thinking)
            self.assertEqual(headers["X-Device-Connection-Created"], "false")
            transport.release.set()
            worker.join(timeout=3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(len(result), 1)
            status, reply = result[0]
            self.assertEqual(status, 200)
            self.assertEqual(reply["turn_id"], thinking["active_turn_id"])
            self.assertEqual(reply["device"]["state"], "ready")
            self.assertEqual(self.request(path=reply["audio"]["url"])[0], 200)
        finally:
            transport.release.set()
            worker.join(timeout=3)


class HubFailureTests(unittest.TestCase):
    def test_device_connection_is_not_used_as_a_voice_session(self):
        class RecordingTransport(MockTransport):
            def exchange(self, turn):
                self.last_input = turn
                return super().exchange(turn)

        transport = RecordingTransport()
        hub = MacHub(transport, AudioStore(), clock=FakeClock())
        connection_id = hub.device.connect()["connection_id"]
        hub.run_turn(TurnInput(text="hello"), connection_id)
        self.assertIsNone(transport.last_input.session_id)
        hub.device.event(connection_id, "cancel")
        hub.run_turn(TurnInput(text="again", session_id="caller-session"), connection_id)
        self.assertEqual(transport.last_input.session_id, "caller-session")
        self.assertNotEqual(transport.last_input.session_id, connection_id)

    def test_unavailable_dot_transport_sets_device_error_without_audio(self):
        store = AudioStore()
        hub = MacHub(UnsupportedDotTransport(), store, clock=FakeClock())
        connection_id = hub.device.connect()["connection_id"]
        with self.assertRaises(TransportUnavailable):
            hub.run_turn(TurnInput(text="hello"), connection_id)
        snapshot = hub.device.snapshot()
        self.assertEqual(snapshot["state"], "error")
        self.assertIsNone(snapshot["active_turn_id"])
        self.assertTrue(snapshot["last_error"])
        self.assertFalse(snapshot["dot_connected"])
        self.assertEqual(len(store._items), 0)


if __name__ == "__main__":
    unittest.main()

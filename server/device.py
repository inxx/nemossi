"""One simulated Nemossi device; connections are not dot voice sessions."""

import re
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Dict, Optional, Tuple

from .protocol import RequestError


@dataclass(frozen=True)
class TurnTicket:
    connection_id: str
    turn_id: str
    generation: int
    started_at: float


class DeviceLifecycle:
    LEASE_SECONDS = 60
    # Up to 15 seconds of audio, plus 5 seconds for capture setup/upload.
    DEADLINES = {"listening": 20, "thinking": 20, "ready": 20, "speaking": 20}
    EVENTS = {"heartbeat", "listen", "cancel", "playback_start", "playback_end", "playback_error", "recover"}

    def __init__(self, clock: Optional[Callable[[], float]] = None,
                 discard_audio: Optional[Callable[[str], None]] = None) -> None:
        self._clock = clock or time.monotonic
        self._discard_audio = discard_audio or (lambda key: None)
        self._lock = threading.RLock()
        self._connection_id = None  # type: Optional[str]
        self._turn_id = None  # type: Optional[str]
        self._audio_id = None  # type: Optional[str]
        self._state = "offline"
        self._revision = 0
        self._generation = 0
        self._last_event = "initial"
        self._last_error = None  # type: Optional[str]
        self._last_seen = 0.0
        self._deadline = None  # type: Optional[float]

    @staticmethod
    def _id(value: Any) -> None:
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{32}", value):
            raise RequestError(400, "invalid_device_request", "Device identifiers must be 32 lowercase hexadecimal characters.")

    def _snapshot(self) -> Dict[str, Any]:
        return {"connected": self._connection_id is not None, "connection_id": self._connection_id,
                "revision": self._revision, "state": self._state, "active_turn_id": self._turn_id,
                "last_event": self._last_event, "last_error": self._last_error,
                "device_kind": "nemossi_simulator", "hardware_tested": False, "dot_connected": False}

    def _invalidate(self) -> None:
        self._generation += 1
        self._turn_id = None
        if self._audio_id is not None:
            self._discard_audio(self._audio_id)
            self._audio_id = None

    def _set(self, state: str, event: str, error: Optional[str] = None) -> None:
        self._state, self._last_event, self._last_error = state, event, error
        self._revision += 1
        duration = self.DEADLINES.get(state)
        self._deadline = self._clock() + duration if duration is not None else None

    def _expire(self) -> None:
        now = self._clock()
        if self._connection_id is not None and now - self._last_seen >= self.LEASE_SECONDS:
            self._invalidate()
            self._connection_id = None
            self._set("offline", "lease_expired", "lease_expired")
        elif self._deadline is not None and now >= self._deadline:
            error = self._state + "_timeout"
            self._invalidate()
            self._set("error", "timeout", error)

    def _connection(self, connection_id: str) -> None:
        self._id(connection_id)
        self._expire()
        if connection_id != self._connection_id:
            raise RequestError(409, "connection_stale", "The device connection is no longer active. Connect again.")

    def _matching_turn(self, turn_id: Optional[str]) -> None:
        if turn_id is not None:
            self._id(turn_id)
        if turn_id is None or turn_id != self._turn_id:
            raise RequestError(409, "turn_stale", "The device turn is no longer active.")

    def _state_is(self, *states: str) -> None:
        if self._state not in states:
            raise RequestError(409, "invalid_transition", "The event is not valid in the current device state.")

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            self._expire()
            return self._snapshot()

    def connect_result(self) -> Tuple[Dict[str, Any], bool]:
        with self._lock:
            self._expire()
            if self._connection_id is not None:
                return self._snapshot(), False
            self._invalidate()
            self._connection_id = uuid.uuid4().hex
            self._last_seen = self._clock()
            self._set("idle", "connect")
            return self._snapshot(), True

    def connect(self) -> Dict[str, Any]:
        return self.connect_result()[0]

    def disconnect(self, connection_id: str) -> Dict[str, Any]:
        with self._lock:
            self._connection(connection_id)
            self._invalidate()
            self._connection_id = None
            self._set("offline", "disconnect")
            return self._snapshot()

    def event(self, connection_id: str, event: str, turn_id: Optional[str] = None) -> Dict[str, Any]:
        with self._lock:
            self._connection(connection_id)
            if event not in self.EVENTS:
                raise RequestError(400, "invalid_device_request", "Unknown device event.")
            playback = event.startswith("playback_")
            if not playback and turn_id is not None:
                raise RequestError(400, "invalid_device_request", "This event does not accept a turn_id.")
            if event == "heartbeat":
                self._last_seen = self._clock()
                self._last_event = "heartbeat"
                self._revision += 1
            elif event == "listen":
                self._state_is("idle")
                self._invalidate()
                self._turn_id = uuid.uuid4().hex
                self._set("listening", "listen")
            elif event == "cancel":
                self._invalidate()
                self._set("idle", "cancel")
            elif event == "recover":
                self._state_is("error")
                self._invalidate()
                self._set("idle", "recover")
            else:
                self._matching_turn(turn_id)
                if event == "playback_start":
                    self._state_is("ready")
                    self._set("speaking", event)
                elif event == "playback_end":
                    self._state_is("speaking")
                    self._invalidate()
                    self._set("idle", event)
                else:
                    self._state_is("ready", "speaking")
                    self._invalidate()
                    self._set("error", event, "playback_failed")
            self._last_seen = self._clock()
            return self._snapshot()

    def begin_turn(self, connection_id: str, turn_id: Optional[str] = None,
                   audio_input: bool = False) -> TurnTicket:
        with self._lock:
            self._connection(connection_id)
            if self._state == "idle" and not audio_input:
                if turn_id is not None:
                    self._id(turn_id)
                    raise RequestError(409, "turn_stale", "An idle text input must not refer to an earlier turn.")
                self._invalidate()
                self._turn_id = uuid.uuid4().hex
            else:
                self._state_is("listening")
                self._matching_turn(turn_id)
            self._last_seen = self._clock()
            self._set("thinking", "turn")
            return TurnTicket(connection_id, self._turn_id, self._generation, self._clock())

    def _ticket_current(self, ticket: TurnTicket) -> bool:
        self._expire()
        return (ticket.connection_id == self._connection_id and ticket.turn_id == self._turn_id
                and ticket.generation == self._generation and self._state == "thinking")

    def complete_turn(self, ticket: TurnTicket, publish_audio: Callable[[], str]) -> Tuple[str, Dict[str, Any]]:
        with self._lock:
            if not self._ticket_current(ticket):
                raise RequestError(409, "turn_invalidated", "The turn was cancelled, disconnected or expired.")
            # Cancel and audio publication share this lock; no stale audio can leak.
            self._audio_id = publish_audio()
            self._set("ready", "reply_ready")
            return self._audio_id, self._snapshot()

    def fail_turn(self, ticket: TurnTicket, error_code: str) -> bool:
        with self._lock:
            if not self._ticket_current(ticket):
                return False
            self._invalidate()
            self._set("error", "turn_error", error_code)
            return True

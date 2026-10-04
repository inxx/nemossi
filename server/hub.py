"""Mac mini hub: owns a transport and one independent simulated device."""

import time
import uuid
from typing import Any, Callable, Dict, Optional

from .device import DeviceLifecycle
from .protocol import AudioStore, MAX_RESPONSE_CHARS, MAX_TEXT_CHARS, RequestError, validate_wav
from .transport import SAMPLE_RATE, TransportAdapter, TurnInput, TurnReply


class MacHub:
    def __init__(self, voice_transport: TransportAdapter, audio_store: AudioStore,
                 clock: Optional[Callable[[], float]] = None) -> None:
        self.voice_transport = voice_transport
        self.audio_store = audio_store
        self.clock = clock or time.monotonic
        self.device = DeviceLifecycle(clock=self.clock, discard_audio=audio_store.discard)

    def snapshot(self) -> Dict[str, Any]:
        return {"hub": {"role": "mac_mini", "mode": self.voice_transport.name},
                "device": self.device.snapshot(), "dot_connected": False, "dot_support_status": "unconfirmed",
                "future_peer": {"status": "reserved", "via": "external_relay", "direct_mac_link_assumed": False}}

    def _exchange(self, turn: TurnInput) -> TurnReply:
        reply = self.voice_transport.exchange(turn)
        if len(reply.text) > MAX_RESPONSE_CHARS or len(reply.transcript) > MAX_TEXT_CHARS:
            raise ValueError("Transport returned oversized text")
        reply.text.encode("utf-8")
        reply.transcript.encode("utf-8")
        validate_wav(reply.audio_wav)
        return reply

    def _response(self, turn_id: str, reply: TurnReply, audio_id: str, started: float) -> Dict[str, Any]:
        return {"turn_id": turn_id, "transcript": reply.transcript, "text": reply.text,
                "mode": self.voice_transport.name,
                "audio": {"url": "/api/audio/" + audio_id + ".wav", "sample_rate": SAMPLE_RATE, "format": "wav"},
                "duration_ms": max(0, int((self.clock() - started) * 1000))}

    def legacy_turn(self, turn: TurnInput) -> Dict[str, Any]:
        started = self.clock()
        reply = self._exchange(turn)
        audio_id = self.audio_store.put(reply.audio_wav)
        return self._response(uuid.uuid4().hex, reply, audio_id, started)

    def run_turn(self, turn: TurnInput, connection_id: str,
                 turn_id: Optional[str] = None) -> Dict[str, Any]:
        ticket = self.device.begin_turn(connection_id, turn_id, audio_input=turn.audio_wav is not None)
        try:
            reply = self._exchange(turn)
            audio_id, snapshot = self.device.complete_turn(ticket, lambda: self.audio_store.put(reply.audio_wav))
        except Exception:
            if not self.device.fail_turn(ticket, "transport_error"):
                raise RequestError(409, "turn_invalidated", "The turn was cancelled, disconnected or expired.")
            raise
        response = self._response(ticket.turn_id, reply, audio_id, ticket.started_at)
        response["device"] = snapshot
        return response

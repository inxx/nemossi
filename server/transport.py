"""Transport boundary for the hardware bridge; no external services are used."""

import io
import math
import struct
import wave
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


SAMPLE_RATE = 16000


class TransportUnavailable(RuntimeError):
    """The requested voice transport has no supported implementation here."""


@dataclass(frozen=True)
class TurnInput:
    text: Optional[str] = None
    audio_wav: Optional[bytes] = None
    audio_duration_seconds: Optional[float] = None
    session_id: Optional[str] = None


@dataclass(frozen=True)
class TurnReply:
    transcript: str
    text: str
    audio_wav: bytes


class TransportAdapter(ABC):
    """An explicit exchange boundary, independent of any AI model or account."""

    name = "unavailable"

    @abstractmethod
    def exchange(self, turn: TurnInput) -> TurnReply:
        raise NotImplementedError


def make_demo_tone() -> bytes:
    """Two short synthetic beeps, deliberately not synthesized speech."""
    frames = bytearray()
    total = int(SAMPLE_RATE * 0.48)
    for index in range(total):
        moment = index / SAMPLE_RATE
        audible = moment < 0.18 or 0.28 <= moment < 0.46
        phase = moment if moment < 0.18 else moment - 0.28
        envelope = max(0.0, min(1.0, phase / 0.01, (0.18 - phase) / 0.01))
        value = int(3800 * envelope * math.sin(2 * math.pi * 660 * moment)) if audible else 0
        frames.extend(struct.pack("<h", value))
    result = io.BytesIO()
    with wave.open(result, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(bytes(frames))
    return result.getvalue()


class MockTransport(TransportAdapter):
    """Local fixture: honest placeholder transcript, response text and a tone."""

    name = "mock"

    def __init__(self) -> None:
        self._tone = make_demo_tone()

    def exchange(self, turn: TurnInput) -> TurnReply:
        if turn.text is not None:
            transcript = turn.text
            preview = turn.text[:180] + ("…" if len(turn.text) > 180 else "")
            response = "네모씨 데모: “{}” 입력을 받았어요. ".format(preview)
        else:
            duration = turn.audio_duration_seconds or 0.0
            transcript = "데모 음성 입력 ({:.2f}초, 실제 음성 인식 아님)".format(duration)
            response = "네모씨 데모: {:.2f}초의 음성을 받았어요. ".format(duration)
        response += "기존 dot 음성 통화 연결은 아직 확인되지 않았어요. 재생 소리는 합성 데모 비프음입니다."
        return TurnReply(transcript=transcript, text=response, audio_wav=self._tone)


class UnsupportedDotTransport(TransportAdapter):
    """Placeholder until an official external-device dot audio transport is confirmed."""

    name = "dot_unavailable"
    message = (
        "dot voice transport is unavailable: an official external-device audio "
        "transport has not been confirmed. Use --transport mock for the local demo."
    )

    def exchange(self, turn: TurnInput) -> TurnReply:
        raise TransportUnavailable(self.message)

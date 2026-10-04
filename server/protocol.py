"""Bounded request/audio protocol primitives shared by the HTTP layer and hub."""

import struct
import threading
import time
import uuid
from collections import OrderedDict
from typing import Callable, Optional, Tuple

from .transport import SAMPLE_RATE


MAX_TEXT_CHARS = 1200
MAX_JSON_BYTES = 8192
MAX_AUDIO_SECONDS = 15
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * MAX_AUDIO_SECONDS + 4096
MAX_RESPONSE_CHARS = 2400
AUDIO_TTL_SECONDS = 120
MAX_STORED_AUDIO = 64


class RequestError(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status, self.code, self.message = status, code, message


def validate_wav(payload: bytes) -> float:
    """Validate the complete RIFF file before handing any audio to a transport."""
    invalid = RequestError(400, "invalid_wav", "A complete PCM WAV file is required.")
    if len(payload) < 44 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise invalid
    if struct.unpack_from("<I", payload, 4)[0] != len(payload) - 8:
        raise invalid
    offset = 12
    fmt = None
    data = None
    while offset < len(payload):
        if len(payload) - offset < 8:
            raise invalid
        tag, length = struct.unpack_from("<4sI", payload, offset)
        offset += 8
        end = offset + length
        padded_end = end + (length % 2)
        if padded_end > len(payload):
            raise invalid
        if tag == b"fmt ":
            if fmt is not None or length not in (16, 18):
                raise invalid
            fmt = payload[offset:end]
            if length == 18 and fmt[16:] != b"\x00\x00":
                raise invalid
        elif tag == b"data":
            if data is not None:
                raise invalid
            data = payload[offset:end]
        offset = padded_end
    if fmt is None or data is None:
        raise invalid
    encoding, channels, rate, byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if (encoding, channels, rate, bits, block_align, byte_rate) != (1, 1, SAMPLE_RATE, 16, 2, SAMPLE_RATE * 2):
        raise RequestError(422, "unsupported_audio_format", "Use mono PCM16 WAV at 16000 Hz.")
    if not data or len(data) % 2:
        raise invalid
    duration = len(data) / (SAMPLE_RATE * 2)
    if duration > MAX_AUDIO_SECONDS:
        raise RequestError(413, "audio_too_long", "Audio must be 15 seconds or shorter.")
    return duration


class AudioStore:
    """Short-lived audio, with a fixed count bound and no filesystem persistence."""

    def __init__(self, ttl: float = AUDIO_TTL_SECONDS, capacity: int = MAX_STORED_AUDIO,
                 clock: Optional[Callable[[], float]] = None) -> None:
        if ttl <= 0 or capacity <= 0:
            raise ValueError("Audio store limits must be positive")
        self.ttl, self.capacity = ttl, capacity
        self._clock = clock or (lambda: time.monotonic())
        self._items = OrderedDict()  # type: OrderedDict[str, Tuple[float, bytes]]
        self._lock = threading.Lock()

    def _expire(self, now: float) -> None:
        while self._items:
            key, (expiry, _) = next(iter(self._items.items()))
            if expiry > now:
                break
            del self._items[key]

    def put(self, payload: bytes) -> str:
        if len(payload) > MAX_AUDIO_BYTES:
            raise ValueError("Transport returned oversized audio")
        with self._lock:
            now = self._clock()
            self._expire(now)
            while len(self._items) >= self.capacity:
                self._items.popitem(last=False)
            key = uuid.uuid4().hex
            self._items[key] = (now + self.ttl, payload)
            return key

    def get(self, key: str) -> Optional[bytes]:
        with self._lock:
            self._expire(self._clock())
            entry = self._items.get(key)
            return entry[1] if entry else None


    def discard(self, key: str) -> None:
        with self._lock:
            self._items.pop(key, None)

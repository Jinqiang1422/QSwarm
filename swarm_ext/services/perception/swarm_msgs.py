#!/usr/bin/env python3
# ruff: noqa: RUF003
"""Perception protocol v2: bounded, source/session/message-isolated TUNNEL JSON.

UDP adds a big-endian uint16 payload type (42003). The 25-byte TUNNEL header is
version:u8, session:u64, message:u32, index:u16, count:u16, length:u32, crc32:u32.
All integers use network byte order. See docs/PERCEPTION_PROTOCOL_V2.md.
"""

from __future__ import annotations

import itertools
import json
import math
import secrets
import struct
import time
import zlib

PT_V2 = 42003
HEADER = struct.Struct("!BQ IHHII")
TUNNEL_PAYLOAD_MAX = 128
CHUNK_DATA_MAX = TUNNEL_PAYLOAD_MAX - HEADER.size
MAX_MESSAGE = 65536
MAX_CONTEXTS = 64
MAX_PER_SOURCE = 8
TTL = 2.0
_DEFAULT_SESSION = secrets.randbits(64)
_DEFAULT_IDS = itertools.count()


class Reassembler:
    """Bounded v2 reassembly; the caller supplies the transport source identity."""

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.messages = {}
        self.completed = {}
        self.rejected = self.expired = self.duplicates = 0

    @property
    def buffered_bytes(self):
        return sum(len(message["body"]) for message in self.messages.values())

    def expire(self):
        now = self.clock()
        for key, message in list(self.messages.items()):
            if now - message["started"] >= TTL:
                del self.messages[key]
                self.expired += 1
        self.completed = {key: at for key, at in self.completed.items() if now - at < TTL}

    def feed(self, payload_type: int, payload: bytes, source="local"):
        self.expire()
        if payload_type != PT_V2 or not HEADER.size <= len(payload) <= TUNNEL_PAYLOAD_MAX:
            self.rejected += 1
            return None
        version, session, seq, index, count, size, checksum = HEADER.unpack_from(payload)
        if (
            version != 2
            or not 0 < size <= MAX_MESSAGE
            or count != (size + CHUNK_DATA_MAX - 1) // CHUNK_DATA_MAX
            or index >= count
            or len(payload) - HEADER.size != min(CHUNK_DATA_MAX, size - index * CHUNK_DATA_MAX)
        ):
            self.rejected += 1
            return None
        key = source, session, seq
        if key in self.completed:
            self.duplicates += 1
            return None
        if key not in self.messages:
            if (
                len(self.messages) >= MAX_CONTEXTS
                or sum(k[0] == source for k in self.messages) >= MAX_PER_SOURCE
            ):
                self.rejected += 1
                return None
            self.messages[key] = {
                "body": bytearray(size),
                "received": set(),
                "crc": checksum,
                "count": count,
                "started": self.clock(),
            }
        message = self.messages[key]
        if len(message["body"]) != size or message["crc"] != checksum or message["count"] != count:
            del self.messages[key]
            self.rejected += 1
            return None
        chunk = payload[HEADER.size :]
        offset = index * CHUNK_DATA_MAX
        if index in message["received"]:
            if message["body"][offset : offset + len(chunk)] != chunk:
                del self.messages[key]
                self.rejected += 1
            else:
                self.duplicates += 1
            return None
        message["body"][offset : offset + len(chunk)] = chunk
        message["received"].add(index)
        if len(message["received"]) != count:
            return None
        del self.messages[key]
        if zlib.crc32(message["body"]) != checksum:
            self.rejected += 1
            return None
        result = decode_payload(message["body"])
        if result is None:
            self.rejected += 1
            return None
        if len(self.completed) >= 512:
            del self.completed[min(self.completed, key=self.completed.get)]
        self.completed[key] = self.clock()
        return result


# ---------------------------------------------------------------------------
# 构造助手（字段契约集中在此，读写两端共用）
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 航迹紧凑线格式：track/track_batch 中的航迹对象 ↔ 位置式数组
# （单条航迹全字段 JSON >128B，必须紧凑化才能单帧承载）
# 位置序：[track_id, category, lat(1e7 int), lon(1e7 int), alt_msl(dm int|-1),
#          ve(cm/s int), vn(cm/s int), confidence(0-1000), sensor_id, source, time_usec]
# ---------------------------------------------------------------------------

_TRACK_KEYS = (
    "track_id",
    "category",
    "lat",
    "lon",
    "alt_msl",
    "ve",
    "vn",
    "confidence",
    "sensor_id",
    "source",
    "time_usec",
)


def track_to_wire(t: dict) -> list:
    return [
        int(t["track_id"]),
        int(t["category"]),
        round(float(t["lat"]) * 1e7),
        round(float(t["lon"]) * 1e7),
        -1 if t.get("alt_msl") is None else round(float(t["alt_msl"]) * 10),
        round(float(t.get("ve", 0.0)) * 100),
        round(float(t.get("vn", 0.0)) * 100),
        round(float(t.get("confidence", 0.5)) * 1000),
        int(t.get("sensor_id", 0)),
        int(t.get("source", 0)),
        int(t.get("time_usec", 0)),
    ]


def track_from_wire(w: list) -> dict:
    if (
        not isinstance(w, list)
        or len(w) != 11
        or any(type(v) not in (int, float) or not math.isfinite(v) or int(v) != v for v in w)
        or any(not -2147483648 <= v <= 2147483647 for v in w[:10])
        or not 0 <= w[0] <= 2147483647
        or w[1] < 0
        or abs(w[2]) > 900000000
        or abs(w[3]) > 1800000000
        or not 0 <= w[7] <= 1000
        or not 0 <= w[8] <= 2147483647
        or not 0 <= w[10] <= 9007199254740991
    ):
        raise ValueError("invalid track fields")
    return {
        "track_id": int(w[0]),
        "category": int(w[1]),
        "lat": w[2] / 1e7,
        "lon": w[3] / 1e7,
        "alt_msl": None if w[4] == -1 else w[4] / 10.0,
        "ve": w[5] / 100.0,
        "vn": w[6] / 100.0,
        "confidence": w[7] / 1000.0,
        "sensor_id": int(w[8]),
        "source": int(w[9]),
        "time_usec": int(w[10]),
    }


def _body(kind: str, obj) -> bytes:
    if kind == "track":
        obj = track_to_wire(obj)
    elif kind == "track_batch":
        obj = {
            "t": int(obj.get("time_usec", 0)),
            "tracks": [track_to_wire(t) for t in obj["tracks"]],
        }
    return json.dumps(
        {"kind": kind, "data": obj}, ensure_ascii=False, allow_nan=False, separators=(",", ":")
    ).encode("utf-8")


def encode(kind: str, obj, seq: int | None = None, *, session: int = _DEFAULT_SESSION) -> list:
    """One complete v2 message. Use Encoder for automatically split track batches."""
    body = _body(kind, obj)
    if not 0 < len(body) <= MAX_MESSAGE:
        raise ValueError("message exceeds 64 KiB")
    seq = next(_DEFAULT_IDS) if seq is None else seq
    if not 0 <= seq <= 0xFFFFFFFF:
        raise ValueError("message ID exhausted; start a new sender session")
    count = (len(body) + CHUNK_DATA_MAX - 1) // CHUNK_DATA_MAX
    crc = zlib.crc32(body)
    return [
        (
            PT_V2,
            HEADER.pack(2, session, seq, index, count, len(body), crc)
            + body[index * CHUNK_DATA_MAX : (index + 1) * CHUNK_DATA_MAX],
        )
        for index in range(count)
    ]


class Encoder:
    def __init__(self, session=None):
        self.session = secrets.randbits(64) if session is None else session
        self.sequence = 0

    def encode(self, kind, obj):
        if kind == "track_batch" and len(_body(kind, obj)) > MAX_MESSAGE:
            tracks = obj["tracks"]
            if len(tracks) < 2:
                raise ValueError("single track exceeds message budget")
            middle = len(tracks) // 2
            return self.encode(kind, {**obj, "tracks": tracks[:middle]}) + self.encode(
                kind, {**obj, "tracks": tracks[middle:]}
            )
        if self.sequence > 0xFFFFFFFF:
            self.session = secrets.randbits(64)
            self.sequence = 0
        frames = encode(kind, obj, self.sequence, session=self.session)
        self.sequence += 1
        return frames


def decode_payload(raw: bytes):
    try:
        msg = json.loads(raw.decode("utf-8"))
        if not isinstance(msg, dict):
            return None
        kind, data = msg.get("kind"), msg.get("data")
        if kind == "track":
            data = track_from_wire(data)
        elif (
            kind == "track_batch"
            and isinstance(data, dict)
            and isinstance(data.get("tracks"), list)
            and type(data.get("t")) in (int, float)
            and 0 <= data["t"] <= 9007199254740991
            and int(data["t"]) == data["t"]
        ):
            data = {
                "time_usec": data.get("t", 0),
                "tracks": [track_from_wire(w) for w in data["tracks"]],
            }
        elif kind not in ("heartbeat", "plan_meta") or not isinstance(data, dict):
            return None
        return kind, data
    except (ValueError, UnicodeDecodeError, TypeError, IndexError, OverflowError):
        return None


def make_heartbeat(
    tracks_active: int, fps: float, camera_channels: int, status: int = 0, time_usec: int = 0
) -> dict:
    return {
        "tracks_active": int(tracks_active),
        "fps_x10": round(fps * 10),
        "camera_channels": int(camera_channels),
        "status": int(status),
        "time_usec": int(time_usec),
    }


def make_track(
    track_id: int,
    category: int,
    lat: float,
    lon: float,
    alt_msl: float | None = None,
    ve: float = 0.0,
    vn: float = 0.0,
    confidence: float = 0.5,
    sensor_id: int = 0,
    source: int = 0,
    time_usec: int = 0,
) -> dict:
    return {
        "track_id": int(track_id),
        "category": int(category),
        "lat": round(float(lat), 7),
        "lon": round(float(lon), 7),
        "alt_msl": None if alt_msl is None else round(float(alt_msl), 1),
        "ve": round(float(ve), 2),
        "vn": round(float(vn), 2),
        "confidence": round(float(confidence), 3),
        "sensor_id": int(sensor_id),
        "source": int(source),
        "time_usec": int(time_usec),
    }


def make_track_batch(tracks: list, time_usec: int = 0) -> dict:
    return {"tracks": list(tracks), "time_usec": int(time_usec)}


def make_plan_meta(plan_id: int, op: int, geojson: dict) -> dict:
    return {"plan_id": int(plan_id), "op": int(op), "geojson": geojson}

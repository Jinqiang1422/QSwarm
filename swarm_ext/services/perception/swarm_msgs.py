#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WP3.7 感知消息契约 — TUNNEL JSON 编解码（单源事实）

链路：感知服务 ──MAVLink TUNNEL(#385)──► QGC SwarmPerceptionBridge
载荷：UTF-8 JSON，单帧 ≤128B 直接发送，超长按 126B 分片重组。

payload_type 约定（与 mavlink/swarm_perception.xml 注释一致）：
    42000 单帧（start+end）  42001 中间帧   42002 尾帧
分片头：每帧首字节为 seq（0~255 滚动），仅多帧时有意义；单帧固定 0。

消息 kind：
    heartbeat    服务心跳 {tracks_active, fps_x10, camera_channels, status}
    track        单条航迹 {track_id, category, lat, lon, alt_msl, ve, vn,
                           confidence, sensor_id, source, time_usec}
    track_batch  批量航迹 {tracks: [...], time_usec}
    plan_meta    计划元数据 {plan_id, op, geojson}

本模块同时被 QGC 侧测试脚本与感知服务使用，保持零第三方依赖。
"""

import json

PT_SINGLE = 42000
PT_MIDDLE = 42001
PT_LAST   = 42002

TUNNEL_PAYLOAD_MAX = 128      # MAVLink TUNNEL 载荷上限
CHUNK_DATA_MAX     = 126      # 首字节留给 seq
_SEQ_MASK          = 0xFF


class Reassembler:
    """接收侧重组器：feed(payload_type, payload) -> (kind, obj) | None"""

    def __init__(self):
        self._buf = bytearray()
        self._open = False

    def feed(self, payload_type: int, payload: bytes):
        if payload_type == PT_SINGLE:
            self._buf.clear()
            self._open = False
            return self._decode(payload[1:])
        if payload_type == PT_MIDDLE:
            self._open = True
            self._buf += payload[1:]
            return None
        if payload_type == PT_LAST:
            if self._open:
                self._buf += payload[1:]
                data = bytes(self._buf)
            else:                       # 丢了首帧的尾帧，按单帧尝试
                data = payload[1:]
            self._buf.clear()
            self._open = False
            return self._decode(data)
        return None

    @staticmethod
    def _decode(raw: bytes):
        return decode_payload(raw)


# ---------------------------------------------------------------------------
# 构造助手（字段契约集中在此，读写两端共用）
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 航迹紧凑线格式：track/track_batch 中的航迹对象 ↔ 位置式数组
# （单条航迹全字段 JSON >128B，必须紧凑化才能单帧承载）
# 位置序：[track_id, category, lat(1e7 int), lon(1e7 int), alt_msl(dm int|-1),
#          ve(cm/s int), vn(cm/s int), confidence(0-1000), sensor_id, source, time_usec]
# ---------------------------------------------------------------------------

_TRACK_KEYS = ("track_id", "category", "lat", "lon", "alt_msl",
               "ve", "vn", "confidence", "sensor_id", "source", "time_usec")


def track_to_wire(t: dict) -> list:
    return [
        int(t["track_id"]), int(t["category"]),
        int(round(float(t["lat"]) * 1e7)), int(round(float(t["lon"]) * 1e7)),
        -1 if t.get("alt_msl") is None else int(round(float(t["alt_msl"]) * 10)),
        int(round(float(t.get("ve", 0.0)) * 100)),
        int(round(float(t.get("vn", 0.0)) * 100)),
        int(round(float(t.get("confidence", 0.5)) * 1000)),
        int(t.get("sensor_id", 0)), int(t.get("source", 0)),
        int(t.get("time_usec", 0)),
    ]


def track_from_wire(w: list) -> dict:
    return {
        "track_id": int(w[0]), "category": int(w[1]),
        "lat": w[2] / 1e7, "lon": w[3] / 1e7,
        "alt_msl": None if w[4] < 0 else w[4] / 10.0,
        "ve": w[5] / 100.0, "vn": w[6] / 100.0,
        "confidence": w[7] / 1000.0,
        "sensor_id": int(w[8]), "source": int(w[9]),
        "time_usec": int(w[10]),
    }


def encode(kind: str, obj, seq: int = 0) -> list:
    """编码入口（重写）：track/track_batch 自动转紧凑线格式。"""
    if kind == "track":
        obj = track_to_wire(obj)
    elif kind == "track_batch":
        obj = {"t": int(obj.get("time_usec", 0)),
               "tracks": [track_to_wire(t) for t in obj["tracks"]]}
    body = json.dumps({"kind": kind, "data": obj},
                      ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(body) <= CHUNK_DATA_MAX:
        return [(PT_SINGLE, bytes([seq & _SEQ_MASK]) + body)]
    frames = []
    off = 0
    while off < len(body):
        chunk = body[off:off + CHUNK_DATA_MAX]
        off += len(chunk)
        pt = PT_MIDDLE if off < len(body) else PT_LAST
        frames.append((pt, bytes([seq & _SEQ_MASK]) + chunk))
    return frames


def decode_payload(raw: bytes):
    """重组完成后的 JSON → (kind, obj)；track/track_batch 自动还原为字典。"""
    try:
        msg = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return None
    kind, data = msg.get("kind"), msg.get("data")
    if kind == "track" and isinstance(data, list):
        data = track_from_wire(data)
    elif kind == "track_batch" and isinstance(data, dict) and "tracks" in data:
        data = {"time_usec": data.get("t", 0),
                "tracks": [track_from_wire(w) for w in data["tracks"]]}
    return kind, data


def make_heartbeat(tracks_active: int, fps: float, camera_channels: int,
                   status: int = 0, time_usec: int = 0) -> dict:
    return {
        "tracks_active": int(tracks_active),
        "fps_x10": int(round(fps * 10)),
        "camera_channels": int(camera_channels),
        "status": int(status),
        "time_usec": int(time_usec),
    }


def make_track(track_id: int, category: int, lat: float, lon: float,
               alt_msl: float = None, ve: float = 0.0, vn: float = 0.0,
               confidence: float = 0.5, sensor_id: int = 0, source: int = 0,
               time_usec: int = 0) -> dict:
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

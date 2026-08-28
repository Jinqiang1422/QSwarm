#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""
WP3.2 检测跟踪服务 — 主进程

运行形态（对应决策 D3「感知先行服务化」，第一个微服务验证点）：
    python3 service.py --scenario scenario.json --out udp://127.0.0.1:15600
    python3 service.py --scenario scenario.json --out file://tracks.jsonl   # 离线/CI
    python3 service.py --scenario scenario.json --dry-run                   # 仅校验场景

场景 JSON（示例见 sim/scenarios/06_perception_feed.json）：
    {
      "duration_s": 30, "fps": 5,
      "cameras":  [{"sensor_id":1, "lat":..., "lon":..., "alt_m":80,
                    "heading_deg":0, "gimbal_pitch_deg":55, "hfov_deg":60}],
      "truths":   [{"category":"vehicle", "waypoints":[[t,lat,lon], ...]}, ...],
      "detector": {"type":"simulated", "pos_noise_px":3, "miss_prob":0.05}
    }

输出：每帧 track_batch（ confirmed/coasting 航迹，WGS84 ）+ 1Hz heartbeat，
经 swarm_msgs 编码分片后 UDP 发送（真实部署由 mavlink-router/直连 QGC 的
TUNNEL 通道承载；UDP JSON 帧 = TUNNEL 载荷字节，QGC 侧按同一契约重组）。
"""

import argparse
import json
import socket
import sys
import time

import swarm_msgs
from detector import CameraPose, OnnxDetector, SimulatedDetector, latlon_to_local, pixel_to_geo
from tracker import MultiObjectTracker


# ENU→WGS84 反算（参考点为场景首相机位置，与 QGC 侧参考点协议一致）
def local_to_geo(ref_lat, ref_lon, east, north):
    import math

    lat = ref_lat + north / 110540.0
    lat_avg = math.radians((ref_lat + lat) / 2.0)
    lon = ref_lon + east / (math.cos(lat_avg) * 111320.0)
    return lat, lon


class UdpSink:
    def __init__(self, url: str):
        host, port = url.replace("udp://", "").rsplit(":", 1)
        self._addr = (host, int(port))
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, payload_type: int, payload: bytes):
        # UDP 帧格式 = [payload_type:uint16be][payload]，QGC 侧按同格式解
        self._sock.sendto(payload_type.to_bytes(2, "big") + payload, self._addr)

    def close(self):
        self._sock.close()


class FileSink:
    def __init__(self, url: str):
        self._fp = open(url.replace("file://", ""), "w", encoding="utf-8")  # noqa: SIM115

    def send(self, payload_type: int, payload: bytes):
        self._fp.write(json.dumps({"pt": payload_type, "payload_hex": payload.hex()}) + "\n")

    def close(self):
        self._fp.close()


def run_scenario(scn: dict, sink, realtime: bool = False, verbose: bool = True) -> dict:
    duration = float(scn.get("duration_s", 30))
    fps = float(scn.get("fps", 5))
    frames = int(duration * fps)
    dt = 1.0 / fps

    det_cfg = scn.get("detector", {"type": "simulated"})
    if det_cfg.get("type", "simulated") == "simulated":
        detector = SimulatedDetector(
            scn.get("truths", []),
            pos_noise_px=float(det_cfg.get("pos_noise_px", 3.0)),
            miss_prob=float(det_cfg.get("miss_prob", 0.05)),
            false_prob=float(det_cfg.get("false_prob", 0.02)),
        )
    else:
        detector = OnnxDetector(det_cfg["model_path"])

    cams = [
        CameraPose(
            lat=c["lat"],
            lon=c["lon"],
            alt_m=c["alt_m"],
            heading_deg=c.get("heading_deg", 0),
            gimbal_pitch_deg=c.get("gimbal_pitch_deg", 55),
            hfov_deg=c.get("hfov_deg", 60),
            sensor_id=c.get("sensor_id", 0),
        )
        for c in scn.get("cameras", [])
    ]
    if not cams:
        raise ValueError("场景缺少 cameras")
    ref_lat, ref_lon = cams[0].lat, cams[0].lon  # ENU 参考点协议

    trackers = {cam.sensor_id: MultiObjectTracker() for cam in cams}
    seq = 0
    t0 = time.time()
    stats = {"frames": 0, "detections": 0, "track_msgs": 0, "heartbeats": 0, "bytes": 0}

    for f in range(frames):
        scenario_usec = int(f * dt * 1e6)  # 场景时间（真值插值用）
        wire_usec = int(time.time() * 1e6)  # 墙钟时间（报文时间戳/时延测量用）
        all_tracks = []
        for cam in cams:
            cam.time_usec = scenario_usec
            dets = detector.detect(f, cam)
            stats["detections"] += len(dets)
            meas = []
            for d in dets:
                geo = pixel_to_geo(d.x_px, d.y_px, cam)
                if geo is None:
                    continue
                e, n = latlon_to_local(ref_lat, ref_lon, geo[0], geo[1])
                meas.append((e, n, d.category, d.confidence, d.sensor_id))
            active = trackers[cam.sensor_id].update(meas, scenario_usec, dt=dt)
            for tr in active:
                lat, lon = local_to_geo(ref_lat, ref_lon, tr.east, tr.north)
                all_tracks.append(
                    swarm_msgs.make_track(
                        tr.id,
                        tr.category,
                        lat,
                        lon,
                        ve=tr.ve,
                        vn=tr.vn,
                        confidence=tr.confidence,
                        sensor_id=cam.sensor_id,
                        source=0,
                        time_usec=wire_usec,
                    )
                )
        if all_tracks:
            for pt, payload in swarm_msgs.encode(
                "track_batch", swarm_msgs.make_track_batch(all_tracks, wire_usec), seq
            ):
                sink.send(pt, payload)
                stats["bytes"] += len(payload)
            stats["track_msgs"] += 1
            seq = (seq + 1) & 0xFF
        if f % int(fps) == 0:  # 1Hz 心跳
            hb = swarm_msgs.make_heartbeat(len(all_tracks), fps, len(cams), 0, wire_usec)
            for pt, payload in swarm_msgs.encode("heartbeat", hb, seq):
                sink.send(pt, payload)
                stats["bytes"] += len(payload)
            stats["heartbeats"] += 1
        stats["frames"] += 1
        if realtime:
            time.sleep(max(0.0, t0 + (f + 1) * dt - time.time()))
        if verbose and f % int(fps * 5) == 0:
            print(f"[service] t={f * dt:5.1f}s 活动航迹 {len(all_tracks)}", flush=True)
    return stats


def main(argv=None):
    ap = argparse.ArgumentParser(description="QGC Swarm 感知检测跟踪服务")
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--out", default="file://tracks.jsonl", help="udp://host:port 或 file://path")
    ap.add_argument("--realtime", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    with open(args.scenario, encoding="utf-8") as fp:
        scn = json.load(fp)
    for key in ("cameras", "truths"):
        if key not in scn:
            print(f"[service] 场景缺少字段: {key}", file=sys.stderr)
            return 2
    if args.dry_run:
        print(
            f"[service] dry-run 通过：{len(scn['cameras'])} 路相机，"
            f"{len(scn['truths'])} 个真值目标，时长 {scn.get('duration_s', 30)}s"
        )
        return 0

    sink = UdpSink(args.out) if args.out.startswith("udp://") else FileSink(args.out)
    stats = run_scenario(scn, sink, realtime=args.realtime, verbose=not args.quiet)
    sink.close()
    print(
        f"[service] 完成：{stats['frames']} 帧，{stats['detections']} 次检测，"
        f"{stats['track_msgs']} 条航迹报文，{stats['heartbeats']} 次心跳，"
        f"{stats['bytes']} 字节"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

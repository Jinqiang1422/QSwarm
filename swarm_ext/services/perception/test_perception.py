#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WP3.2/3.7 感知服务测试（无 GPU/无 Qt 环境可跑）：
  1. 编解码回环：单帧 / 多帧分片 / 丢首帧容错
  2. 几何互逆：geo_to_pixel 与 pixel_to_geo 往返误差 < 1 m
  3. 跟踪稳定：双目标交叉运动后航迹 ID 不交换、不漂移
  4. 端到端：仿真场景 → 航迹报文 → 重组 → 地理误差 < 8 m、ID 持续
退出码 0 = 全部通过。
"""

import json
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import swarm_msgs
from detector import (CameraPose, SimulatedDetector, pixel_to_geo,
                      geo_to_pixel, latlon_to_local)
from tracker import MultiObjectTracker, GATING_CHI2
from service import run_scenario, local_to_geo, FileSink

PASS = FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS {name} {detail}")
    else:
        FAIL += 1
        print(f"  FAIL {name} {detail}")


# ---------------------------------------------------------------------------
print("== 1. TUNNEL JSON 编解码回环 ==")
reas = swarm_msgs.Reassembler()
tr = swarm_msgs.make_track(7, 2, 39.9051234, 116.4054321, ve=1.5, vn=-0.5,
                           confidence=0.87, sensor_id=1, time_usec=123456)
frames = swarm_msgs.encode("track", tr, seq=3)
check("单帧小报文", len(frames) == 1, f"({len(frames)} 帧)")
kind, obj = reas.feed(*frames[0])
check("单帧还原", kind == "track" and obj["track_id"] == 7
      and abs(obj["lat"] - 39.9051234) < 1e-7)

big = swarm_msgs.make_track_batch([tr] * 20, 999)     # 必超 128B → 分片
frames = swarm_msgs.encode("track_batch", big, seq=4)
check("大报文分片", len(frames) >= 3, f"({len(frames)} 帧, "
      f"{sum(len(p) for _, p in frames)} 字节)")
check("分片类型序列", frames[0][0] == swarm_msgs.PT_MIDDLE
      and frames[-1][0] == swarm_msgs.PT_LAST)
reas2 = swarm_msgs.Reassembler()
result = None
for pt, payload in frames:
    result = reas2.feed(pt, payload) or result
check("分片重组", result is not None and result[0] == "track_batch"
      and len(result[1]["tracks"]) == 20)
orphan = swarm_msgs.Reassembler().feed(swarm_msgs.PT_LAST, frames[-1][1])
check("丢首帧容错", orphan is None or orphan[0] == "track_batch")

# ---------------------------------------------------------------------------
print("== 2. 像素↔几何互逆 ==")
cam = CameraPose(lat=39.9, lon=116.4, alt_m=80, heading_deg=30,
                 gimbal_pitch_deg=55, hfov_deg=60)
worst = 0.0
for px, py in [(640, 360), (300, 500), (900, 200), (100, 650), (1200, 600)]:
    geo = pixel_to_geo(px, py, cam)
    if geo is None:
        continue
    back = geo_to_pixel(geo[0], geo[1], cam)
    e, n = latlon_to_local(*geo[:2], *pixel_to_geo(back[0], back[1], cam))
    worst = max(worst, math.hypot(e, n))
check("往返误差 <1m", worst < 1.0, f"(最大 {worst:.3f}m)")
check("天底点", pixel_to_geo(640, 720, CameraPose(
    39.9, 116.4, 80, 0, 90)) is not None)
check("朝天射线拒绝", pixel_to_geo(640, 100, CameraPose(
    39.9, 116.4, 80, 0, 5)) is None)

# ---------------------------------------------------------------------------
print("== 3. 跟踪器 ID 稳定（交叉场景） ==")
trk = MultiObjectTracker()
# 两目标相向而行，在 t=10s 交叉
for step in range(100):
    t = step * 0.2
    m = [(0.0 + 2.0 * t, 0.0, 2, 0.9),      # A: 西向东
         (40.0 - 2.0 * t, 0.5, 1, 0.9)]     # B: 东向西（纬度差 0.5m 近平行）
    active = trk.update(m, int(t * 1e6), dt=0.2)
final = {tr.id: (tr.east, tr.north, tr.category) for tr in active}
check("双目标均存活", len(final) == 2, f"({len(final)})")
ids = sorted(final)
pos = [final[i][:2] for i in ids]
check("交叉后位置正确", abs(pos[0][0] - 40.0) < 3 and abs(pos[1][0] - 0.0) < 3,
      f"({pos[0][0]:.1f}, {pos[1][0]:.1f})")
check("类别保持", final[ids[0]][2] == 2 and final[ids[1]][2] == 1)

# 短暂丢失不应删除 confirmed 航迹
trk2 = MultiObjectTracker()
for step in range(30):
    trk2.update([(10.0, 10.0, 2, 0.9)], step * 200000, dt=0.2)
for step in range(4):                       # 丢失 4 帧 < max_misses
    active2 = trk2.update([], (30 + step) * 200000, dt=0.2)
check("coasting 保留", len(active2) == 1 and active2[0].state == "coasting")

# ---------------------------------------------------------------------------
print("== 4. 端到端仿真场景 ==")
scenario = {
    "duration_s": 20, "fps": 5,
    "cameras": [{"sensor_id": 1, "lat": 39.9, "lon": 116.4, "alt_m": 80,
                 "heading_deg": 0, "gimbal_pitch_deg": 55, "hfov_deg": 60}],
    "truths": [
        {"category": "vehicle",
         "waypoints": [[0, 39.90055, 116.40035], [20, 39.90090, 116.40035]]},
        {"category": "person",
         "waypoints": [[0, 39.90030, 116.39985], [20, 39.90030, 116.40015]]},
    ],
    "detector": {"type": "simulated", "pos_noise_px": 2.5,
                 "miss_prob": 0.05, "false_prob": 0.0},
}
with tempfile.TemporaryDirectory() as td:
    sp = os.path.join(td, "scn.json")
    out = os.path.join(td, "tracks.jsonl")
    with open(sp, "w", encoding="utf-8") as fp:
        json.dump(scenario, fp)
    sink = FileSink("file://" + out)
    stats = run_scenario(scenario, sink, verbose=False)
    sink.close()

    reas3 = swarm_msgs.Reassembler()
    batches = []
    for line in open(out, encoding="utf-8"):
        rec = json.loads(line)
        got = reas3.feed(rec["pt"], bytes.fromhex(rec["payload_hex"]))
        if got and got[0] == "track_batch":
            batches.append(got[1])

check("航迹报文流", len(batches) >= 15, f"({len(batches)} 批/{stats['frames']} 帧)")
if batches:
    last = batches[-1]["tracks"]
    by_cat = {}
    for t in last:
        by_cat.setdefault(t["category"], []).append(t)
    err = None
    for t in last:
        if t["category"] == 2:   # vehicle 真值终点 (39.90090, 116.40035)
            e, n = latlon_to_local(39.90090, 116.40035, t["lat"], t["lon"])
            err = math.hypot(e, n)
    check("双目标识别", len(by_cat.get(2, [])) >= 1 and len(by_cat.get(1, [])) >= 1,
          f"(类别分布 {sorted(by_cat)})")
    check("末端地理误差 <8m", err is not None and err < 8.0,
          f"({err if err is None else f'{err:.2f}m'})")
    vids = {t["track_id"] for b in batches[-10:] for t in b["tracks"]}
    check("ID 持续不爆炸", len(vids) <= 6, f"(近10帧出现 {len(vids)} 个 ID)")

print(f"\n感知服务测试: {PASS} PASS / {FAIL} FAIL")
sys.exit(1 if FAIL else 0)

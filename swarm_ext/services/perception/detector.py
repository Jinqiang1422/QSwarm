#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""
WP3.2 检测跟踪服务 — 检测器层

DetectorBase：统一检测接口，输入相机位姿+帧元数据，输出像素级 Detection。
  - SimulatedDetector：仿真数据源。内置真值目标（WGS84 轨迹），按针孔模型
    投影到像素平面，叠加漏检/虚警/定位噪声 —— 用于无 GPU 环境的算法验证
    与端到端链路测试。
  - OnnxDetector：真实部署入口（ultralytics YOLO 类，ONNX/TensorRT），
    沙箱无 GPU 时给出明确错误提示；接口与模拟检测器完全一致，可热替换。

像素→地理坐标的解算公式与 QGC 侧 SwarmVideoHub::pixelToGeo 保持同一套
（单源公式，两端独立实现，测试交叉验证）。
"""

import math
from dataclasses import dataclass

# 类别表（与 mavlink/swarm_perception.xml SWARM_TARGET_CATEGORY 对齐）
CATEGORY_NAMES = {
    0: "unknown",
    1: "person",
    2: "vehicle",
    3: "boat",
    4: "animal",
    5: "fire",
    6: "smoke",
}
CATEGORY_IDS = {v: k for k, v in CATEGORY_NAMES.items()}

EARTH_R = 6371000.0


def destination_point(lat, lon, bearing_deg, dist_m):
    """与 SwarmTypes::destinationPoint 同一公式。"""
    brg = math.radians(bearing_deg)
    lat1 = math.radians(lat)
    d = dist_m / EARTH_R
    lat2 = math.asin(math.sin(lat1) * math.cos(d) + math.cos(lat1) * math.sin(d) * math.cos(brg))
    lon2 = math.radians(lon) + math.atan2(
        math.sin(brg) * math.sin(d) * math.cos(lat1), math.cos(d) - math.sin(lat1) * math.sin(lat2)
    )
    return math.degrees(lat2), math.degrees(lon2)


def latlon_to_local(ref_lat, ref_lon, lat, lon):
    """等距圆柱近似（与 SwarmTypes::latLonToLocalM 同一公式）。"""
    lat_avg = math.radians((ref_lat + lat) / 2.0)
    east = (lon - ref_lon) * math.cos(lat_avg) * 111320.0
    north = (lat - ref_lat) * 110540.0
    return east, north


@dataclass
class CameraPose:
    """一帧对应的相机位姿（仿真/真实通用）。"""

    lat: float  # 相机 WGS84
    lon: float
    alt_m: float  # 相对地面高度（米）
    heading_deg: float  # 机头/云台球面方位（北起顺时针）
    gimbal_pitch_deg: float  # 视轴下倾角（0=水平，90=垂直向下）
    width_px: int = 1280
    height_px: int = 720
    hfov_deg: float = 60.0
    sensor_id: int = 0
    time_usec: int = 0

    @property
    def vfov_deg(self):
        return math.degrees(
            2.0
            * math.atan(
                math.tan(math.radians(self.hfov_deg) / 2.0) * self.height_px / self.width_px
            )
        )


@dataclass
class Detection:
    x_px: float
    y_px: float
    w_px: float
    h_px: float
    category: int
    confidence: float
    sensor_id: int = 0
    time_usec: int = 0


def pixel_to_geo(px: float, py: float, cam: CameraPose):
    """
    像素→地面交点（WGS84）。平地假设 + 针孔模型：
      像素俯角 = 视轴下倾 + (py/H − 0.5) × vfov   （必须 >0，否则射线朝天）
      斜距地距 = alt / tan(像素俯角)
      方位角  = heading + (px/W − 0.5) × hfov
    返回 (lat, lon)；射线不落地返回 None。
    """
    pitch = cam.gimbal_pitch_deg + (py / cam.height_px - 0.5) * cam.vfov_deg
    if pitch <= 1.0:  # 接近水平，交点过远不可靠
        return None
    ground_range = cam.alt_m / math.tan(math.radians(pitch))
    if ground_range > 3000.0:  # 超可靠解算范围
        return None
    yaw = cam.heading_deg + (px / cam.width_px - 0.5) * cam.hfov_deg
    return destination_point(cam.lat, cam.lon, yaw, ground_range)


def geo_to_pixel(lat: float, lon: float, cam: CameraPose):
    """
    地理点→像素（SimulatedDetector 用，pixel_to_geo 的逆）：
      相机系 ENU 偏移 → 地距/方位 → 反解俯角与偏航 → 像素坐标。
    返回 (px, py)；在视场外返回 None。
    """
    east, north = latlon_to_local(cam.lat, cam.lon, lat, lon)
    ground_range = math.hypot(east, north)
    if ground_range < 1e-6:
        return cam.width_px / 2.0, cam.height_px / 2.0  # 正下方
    bearing = math.degrees(math.atan2(east, north))  # 北起
    pitch = math.degrees(math.atan2(cam.alt_m, ground_range))
    rel_yaw = (bearing - cam.heading_deg + 540.0) % 360.0 - 180.0
    px = (rel_yaw / cam.hfov_deg + 0.5) * cam.width_px
    py = ((pitch - cam.gimbal_pitch_deg) / cam.vfov_deg + 0.5) * cam.height_px
    if 0 <= px < cam.width_px and 0 <= py < cam.height_px:
        return px, py
    return None


class DetectorBase:
    def detect(self, frame_id: int, cam: CameraPose) -> list:
        raise NotImplementedError


class SimulatedDetector(DetectorBase):
    """
    仿真检测器：truth 目标（WGS84 航路点折线插值）→ 投影像素 → 加噪声。
    参数：pos_noise_px 定位噪声（像素）、miss_prob 漏检率、false_prob 虚警率/帧。
    """

    def __init__(
        self,
        truths: list,
        pos_noise_px: float = 3.0,
        miss_prob: float = 0.05,
        false_prob: float = 0.02,
        seed: int = 7,
    ):
        import random

        self._rng = random.Random(seed)
        self._truths = truths  # [{category, waypoints:[[t,lat,lon],...]}]
        self._noise = pos_noise_px
        self._miss = miss_prob
        self._false = false_prob

    def _truth_at(self, wps: list, t: float):
        if t <= wps[0][0]:
            return wps[0][1], wps[0][2]
        if t >= wps[-1][0]:
            return wps[-1][1], wps[-1][2]
        for i in range(1, len(wps)):
            if wps[i][0] >= t:
                t0, la0, lo0 = wps[i - 1]
                t1, la1, lo1 = wps[i]
                r = (t - t0) / max(1e-9, t1 - t0)
                return la0 + (la1 - la0) * r, lo0 + (lo1 - lo0) * r
        return wps[-1][1], wps[-1][2]

    def detect(self, frame_id: int, cam: CameraPose) -> list:
        t = cam.time_usec / 1e6
        out = []
        for truth in self._truths:
            if self._rng.random() < self._miss:
                continue
            lat, lon = self._truth_at(truth["waypoints"], t)
            proj = geo_to_pixel(lat, lon, cam)
            if proj is None:
                continue
            px = proj[0] + self._rng.gauss(0, self._noise)
            py = proj[1] + self._rng.gauss(0, self._noise)
            size = max(8.0, 200.0 * 2.0 / max(1.0, cam.alt_m))  # 粗略目标尺度
            out.append(
                Detection(
                    px,
                    py,
                    size,
                    size * 1.4,
                    CATEGORY_IDS.get(truth.get("category", "unknown"), 0),
                    min(0.99, 0.75 + self._rng.random() * 0.2),
                    cam.sensor_id,
                    cam.time_usec,
                )
            )
        if self._rng.random() < self._false:  # 虚警
            out.append(
                Detection(
                    self._rng.uniform(0, cam.width_px),
                    self._rng.uniform(cam.height_px * 0.3, cam.height_px),
                    20,
                    28,
                    0,
                    0.35,
                    cam.sensor_id,
                    cam.time_usec,
                )
            )
        return out


class OnnxDetector(DetectorBase):
    """真实部署入口：ultralytics YOLO 类模型。沙箱无 GPU/模型时明确报错。"""

    def __init__(self, model_path: str, conf_threshold: float = 0.4):
        try:
            from ultralytics import YOLO
        except ImportError as e:
            raise RuntimeError(
                "OnnxDetector 需要 ultralytics（地面 GPU 工作站安装："
                "pip install ultralytics；或使用 SimulatedDetector 做链路验证）"
            ) from e
        self._model = YOLO(model_path)
        self._conf = conf_threshold

    def detect(self, frame_id: int, cam: CameraPose, frame_bgr=None) -> list:
        if frame_bgr is None:
            raise RuntimeError("OnnxDetector 需要真实视频帧（frame_bgr）")
        results = self._model.predict(frame_bgr, conf=self._conf, verbose=False)
        out = []
        for r in results:
            for b in r.boxes:
                x1, y1, x2, y2 = (float(v) for v in b.xyxy[0])
                out.append(
                    Detection(
                        (x1 + x2) / 2,
                        (y1 + y2) / 2,
                        x2 - x1,
                        y2 - y1,
                        int(b.cls),
                        float(b.conf),
                        cam.sensor_id,
                        cam.time_usec,
                    )
                )
        return out

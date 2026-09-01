#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003
"""
WP3.2 检测跟踪服务 — 多目标跟踪器

ENU 局部平面恒速模型 Kalman 滤波 + 全局最近邻（匈牙利）数据关联：
  - 量测：Detection 经 pixel_to_geo 解算后的 ENU 位置 (east, north)
  - 状态：[east, north, ve, vn]，逐传感器独立跟踪器实例
  - 生命周期：tentative（命中<min_hits）→ confirmed → coasting（丢失≤max_misses）→ 删除
  - 关联代价：预测位置与量测的马氏距离（S 矩阵），门限 GATING_CHI2；
    scipy 可用时用 linear_sum_assignment，否则贪心降级（目标数少时等价）

输出：confirmed/coasting 航迹 → swarm_msgs.make_track 兼容字段
（ENU→WGS84 由调用方给参考点换算）。
"""

try:
    import numpy as np

    _HAS_NP = True
except ImportError:  # 极简环境降级：纯 Python 2 状态 α-β 滤波
    np = None
    _HAS_NP = False

try:
    from scipy.optimize import linear_sum_assignment

    _HAS_SCIPY = True
except ImportError:
    _HAS_SCIPY = False

GATING_CHI2 = 9.21  # 2 自由度 99% 门限
SIGMA_MEAS_M = 4.0  # 量测噪声（米，平地解算典型误差）
SIGMA_PROC_MS2 = 1.5  # 过程噪声（加速度）


class _KalmanCV:
    """单目标恒速 Kalman（numpy 路径）。"""

    def __init__(self, e0: float, n0: float):
        self.x = np.array([e0, n0, 0.0, 0.0])  # [e,n,ve,vn]
        self.P = np.diag([25.0, 25.0, 16.0, 16.0])

    @staticmethod
    def _F(dt):
        return np.array([[1, 0, dt, 0], [0, 1, 0, dt], [0, 0, 1, 0], [0, 0, 0, 1]], dtype=float)

    def predict(self, dt: float):
        F = self._F(dt)
        q = SIGMA_PROC_MS2**2
        Q = q * np.array(
            [
                [dt**4 / 4, 0, dt**3 / 2, 0],
                [0, dt**4 / 4, 0, dt**3 / 2],
                [dt**3 / 2, 0, dt**2, 0],
                [0, dt**3 / 2, 0, dt**2],
            ]
        )
        self.x = F @ self.x
        self.P = F @ self.P @ F.T + Q

    def gating_dist(self, e: float, n: float) -> float:
        """马氏距离（量测空间 2 维）。"""
        H = np.array([[1, 0, 0, 0], [0, 1, 0, 0]], dtype=float)
        R = np.eye(2) * SIGMA_MEAS_M**2
        S = H @ self.P @ H.T + R
        y = np.array([e, n]) - H @ self.x
        return float(y @ np.linalg.inv(S) @ y)

    def update(self, e: float, n: float):
        H = np.array([[1, 0, 0, 0], [0, 1, 0, 0]], dtype=float)
        R = np.eye(2) * SIGMA_MEAS_M**2
        S = H @ self.P @ H.T + R
        K = self.P @ H.T @ np.linalg.inv(S)
        y = np.array([e, n]) - H @ self.x
        self.x = self.x + K @ y
        self.P = (np.eye(4) - K @ H) @ self.P


class _AlphaBeta:
    """纯 Python 降级路径：α-β 滤波（无 numpy 时使用）。"""

    def __init__(self, e0: float, n0: float):
        self.e, self.n, self.ve, self.vn = e0, n0, 0.0, 0.0
        self._alpha, self._beta = 0.55, 0.25

    def predict(self, dt: float):
        self.e += self.ve * dt
        self.n += self.vn * dt

    def gating_dist(self, e: float, n: float) -> float:
        d2 = (self.e - e) ** 2 + (self.n - n) ** 2
        return d2 / max(1.0, SIGMA_MEAS_M**2)  # 近似马氏

    def update(self, e: float, n: float, dt: float = 0.2):
        re, rn = e - self.e, n - self.n
        self.e += self._alpha * re
        self.n += self._alpha * rn
        self.ve += self._beta * re / max(dt, 1e-3)
        self.vn += self._beta * rn / max(dt, 1e-3)

    @property
    def x(self):
        return [self.e, self.n, self.ve, self.vn]


class Track:
    _next_id = 1

    def __init__(
        self, e0: float, n0: float, category: int, confidence: float, time_usec: int, sensor_id: int
    ):
        self.id = Track._next_id
        Track._next_id += 1
        self.kf = _KalmanCV(e0, n0) if _HAS_NP else _AlphaBeta(e0, n0)
        self.category = category
        self.confidence = confidence
        self.hits = 1
        self.misses = 0
        self.age = 0
        self.last_time_usec = time_usec
        self.sensor_id = sensor_id

    @property
    def state(self):
        if self.hits < 3:
            return "tentative"
        return "coasting" if self.misses > 0 else "confirmed"

    @property
    def east(self):
        return float(self.kf.x[0])

    @property
    def north(self):
        return float(self.kf.x[1])

    @property
    def ve(self):
        return float(self.kf.x[2])

    @property
    def vn(self):
        return float(self.kf.x[3])


class MultiObjectTracker:
    """单传感器多目标跟踪器。"""

    def __init__(self, min_hits: int = 3, max_misses: int = 5):
        self.tracks = []
        self.min_hits = min_hits
        self.max_misses = max_misses

    def update(self, measurements: list, time_usec: int, dt: float = 0.2) -> list:
        """measurements: [(east, north, category, confidence), ...]，返回活动航迹。"""
        for tr in self.tracks:
            tr.kf.predict(dt)
            tr.age += 1

        assigned_t, assigned_m = set(), set()
        if self.tracks and measurements:
            cost = [[tr.kf.gating_dist(m[0], m[1]) for m in measurements] for tr in self.tracks]
            if _HAS_SCIPY and _HAS_NP:
                import numpy as _np

                rows, cols = linear_sum_assignment(_np.array(cost))
                pairs = zip(rows.tolist(), cols.tolist())  # noqa: B905 - Python 3.8 compatibility
            else:
                pairs = self._greedy(cost)
            for ti, mi in pairs:
                if cost[ti][mi] <= GATING_CHI2:
                    tr = self.tracks[ti]
                    m = measurements[mi]
                    tr.kf.update(m[0], m[1])
                    tr.category = m[2] if m[2] != 0 else tr.category
                    tr.confidence = 0.7 * tr.confidence + 0.3 * m[3]
                    tr.hits += 1
                    tr.misses = 0
                    tr.last_time_usec = time_usec
                    assigned_t.add(ti)
                    assigned_m.add(mi)

        for ti, tr in enumerate(self.tracks):
            if ti not in assigned_t:
                tr.misses += 1
        self.tracks = [
            tr
            for tr in self.tracks
            if tr.misses <= self.max_misses and not (tr.state == "tentative" and tr.misses >= 2)
        ]

        for mi, m in enumerate(measurements):
            if mi not in assigned_m:
                self.tracks.append(
                    Track(m[0], m[1], m[2], m[3], time_usec, sensor_id=m[4] if len(m) > 4 else 0)
                )
        return [tr for tr in self.tracks if tr.state != "tentative"]

    @staticmethod
    def _greedy(cost: list):
        pairs = []
        used_r, used_c = set(), set()
        flat = sorted((cost[r][c], r, c) for r in range(len(cost)) for c in range(len(cost[0])))
        for _value, r, c in flat:
            if r not in used_r and c not in used_c:
                pairs.append((r, c))
                used_r.add(r)
                used_c.add(c)
        return pairs

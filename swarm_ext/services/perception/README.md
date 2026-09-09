# Swarm 感知检测跟踪服务（WP3.2，第一个微服务）

独立进程运行（地面 GPU 工作站或边缘盒），经 TUNNEL JSON 协议向 QGC 上报结构化目标航迹。

## 组成

| 文件 | 职责 |
| --- | --- |
| `swarm_msgs.py` | WP3.7 协议编解码（TUNNEL 分片 + 位置式紧凑线格式），单源契约 |
| `detector.py` | 检测器层：`SimulatedDetector`（仿真/CI）与 `OnnxDetector`（真实部署，ultralytics YOLO 类） |
| `tracker.py` | 多目标跟踪：ENU 恒速 Kalman + 匈牙利关联（scipy，缺省贪心降级）+ 生命周期 |
| `service.py` | 主进程：场景驱动 → 检测 → 跟踪 → 地理解算 → 编码 → UDP/文件输出 |
| `test_perception.py` | 22 项检查：编解码回环 / 几何互逆 / 跟踪稳定 / 端到端精度 |

## 运行

```bash
# 仿真场景（无 GPU 验证链路）
python3 service.py --scenario ../../sim/scenarios/06_perception_feed.json \
    --out udp://127.0.0.1:15600 --realtime

# 离线/CI（写文件供校验）
python3 service.py --scenario ../../sim/scenarios/06_perception_feed.json \
    --out file://tracks.jsonl --quiet

# 真实部署（GPU + ONNX 模型，detector.type=onnx）
python3 service.py --scenario real_feed.json --out udp://<QGC_IP>:15600 --realtime
```

QGC 侧：AI 面板「感知链路」启动（默认 `127.0.0.1:15600`），或 `SwarmManager::startPerception(port)`。
远程接入须设置 `Swarm/perceptionBindAddress` 和非空的 `Swarm/perceptionAllowedSources` 地址白名单。

## 协议（与 mavlink/swarm_perception.xml 语义对应）

```text
UDP 帧 = [payload_type:uint16be=42003][payload，最多128B]
payload = [v2 头部25B][JSON 分片，最多103B]
头部 = version:uint8 + session:uint64 + message_id:uint32
     + fragment_index:uint16 + fragment_count:uint16 + total_length:uint32 + crc32:uint32
kind：heartbeat(1Hz) / track / track_batch / plan_meta
航迹线格式（位置式数组）：
  [track_id, category, lat×1e7, lon×1e7, alt_dm|-1, ve_cm/s, vn_cm/s,
   confidence×1000, sensor_id, source, time_usec]
```

完整字段、字节序、重组限制和升级要求以[感知协议 v2](../../docs/PERCEPTION_PROTOCOL_V2.md)为准。
两端必须同时升级；运行时拒绝旧的 42000/42001/42002 帧。
载荷大小适配 MAVLink TUNNEL(#385)，当前服务与 QGC 感知桥验证的是 UDP 传输。
通过真实 MAVLink 链路转发 TUNNEL 仍须实现相应接线并验证，不能据此视为已完成硬件通道验收。

## 真实检测器接入

`detector.type=onnx` 时加载 ultralytics 模型（`pip install ultralytics`，GPU 推荐 TensorRT 导出）。
像素→地理解算沿用 `pixel_to_geo`（针孔模型+平地假设，与 QGC 侧 `SwarmVideoHub::pixelToGeo`
同一公式，跨语言数值一致经测试验证 ≤1e-3 m）。

## 验收指标

- 端到端时延 ≤500ms（720p，wireLatencyMs 面板可见）
- 航迹地理误差 ≤8m（80m 高度、55° 云台角仿真验证实测 ~1.1m）
- 跟踪 ID 稳定：交叉目标不交换、coasting ≤5 帧保留
- COP 刷新 ≥1Hz（融合节拍 2Hz）

## 真实视频输入配置

每路相机提供 `video_source`，可为本地文件、RTSP/HTTP 地址或整数设备编号；`sensor_id` 必须唯一。`detector.type=onnx` 不要求 `truths`。例如：

```json
{
  "duration_s": 30, "fps": 5,
  "detector": {"type": "onnx", "model_path": "/models/detector.onnx"},
  "cameras": [{"sensor_id": 1, "video_source": "/videos/sample.mp4",
               "lat": 39.9, "lon": 116.4, "alt_m": 80,
               "heading_deg": 0, "gimbal_pitch_deg": 55, "hfov_deg": 60}]
}
```

服务逐帧读取 BGR 图像，将实际宽高用于坐标解算，随后显式传给检测器。正常结束和异常路径都会释放捕获对象；网络流采用 FFmpeg 读取/打开超时并最多重连两次，`reconnect_attempts` 可设为 0–5。文件末尾和无法读取帧会明确报错，不能被计为完成预定时长。

位姿来自配置，适用于固定标定相机或受控录制。移动机载视频仍需按采集时间同步飞行器与云台姿态，当前配置不能替代该同步。超时和单帧缓冲依赖后端支持；目前测试验证取帧、模型入参和清理接线，没有宣称真实相机/模型的端到端精度或时延达标。

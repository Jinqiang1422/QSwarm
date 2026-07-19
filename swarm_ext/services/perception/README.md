# Swarm 感知检测跟踪服务（WP3.2，第一个微服务）

独立进程运行（地面 GPU 工作站或边缘盒），经 TUNNEL JSON 协议向 QGC 上报结构化目标航迹。

## 组成

| 文件 | 职责 |
|---|---|
| `swarm_msgs.py` | WP3.7 协议编解码（TUNNEL 分片 + 位置式紧凑线格式），单源契约 |
| `detector.py` | 检测器层：`SimulatedDetector`（仿真/CI）与 `OnnxDetector`（真实部署，ultralytics YOLO 类） |
| `tracker.py` | 多目标跟踪：ENU 恒速 Kalman + 匈牙利关联（scipy，缺省贪心降级）+ 生命周期 |
| `service.py` | 主进程：场景驱动 → 检测 → 跟踪 → 地理解算 → 编码 → UDP/文件输出 |
| `test_perception.py` | 17 项测试：编解码回环 / 几何互逆 / 跟踪稳定 / 端到端精度 |

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

QGC 侧：AI 面板「感知链路」启动（默认 UDP 15600），或 `SwarmManager::startPerception(port)`。

## 协议（与 mavlink/swarm_perception.xml 语义对应）

```
UDP 帧 = [payload_type:uint16be][payload]
payload = [seq:uint8][JSON 分片 ≤126B]
payload_type：42000 单帧 / 42001 中间帧 / 42002 尾帧
kind：heartbeat(1Hz) / track / track_batch
航迹线格式（位置式数组）：
  [track_id, category, lat×1e7, lon×1e7, alt_dm|-1, ve_cm/s, vn_cm/s,
   confidence×1000, sensor_id, source, time_usec]
```

真实 MAVLink 通道部署时，同一 payload 字节流直接装入 TUNNEL(#385) 消息 payload 字段
（payload_type 同值），QGC 侧解析路径一致。

## 真实检测器接入

`detector.type=onnx` 时加载 ultralytics 模型（`pip install ultralytics`，GPU 推荐 TensorRT 导出）。
像素→地理解算沿用 `pixel_to_geo`（针孔模型+平地假设，与 QGC 侧 `SwarmVideoHub::pixelToGeo`
同一公式，跨语言数值一致经测试验证 ≤1e-3 m）。

## 验收指标

- 端到端时延 ≤500ms（720p，wireLatencyMs 面板可见）
- 航迹地理误差 ≤8m（80m 高度、55° 云台角仿真验证实测 ~1.1m）
- 跟踪 ID 稳定：交叉目标不交换、coasting ≤5 帧保留
- COP 刷新 ≥1Hz（融合节拍 2Hz）

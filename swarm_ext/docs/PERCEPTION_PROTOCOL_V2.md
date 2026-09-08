# 感知协议 v2

本契约由 `src/Swarm/SwarmTunnelProtocol.h` 和
`swarm_ext/services/perception/swarm_msgs.py` 共同实现。QGC 与感知服务必须同时升级。
运行时拒绝旧协议；升级前停止旧服务，升级后重新启动两端。

## 帧布局

UDP 数据报使用大端 `uint16` 类型码 **42003**，后接下表中的 TUNNEL 载荷。
UDP 最大 130 字节；TUNNEL 载荷最大 128 字节。整数均为大端。

| 载荷偏移 | 字段 | 类型 | 约束 |
| --- | --- | --- | --- |
| 0 | version | uint8 | 固定为 2 |
| 1 | session | uint64 | 发送进程每次启动生成新的随机会话号 |
| 9 | message_id | uint32 | 会话内每条完整消息递增；耗尽后更换会话 |
| 13 | fragment_index | uint16 | 从 0 开始 |
| 15 | fragment_count | uint16 | 等于完整长度除以 103 后向上取整 |
| 17 | total_length | uint32 | 完整 UTF-8 JSON 字节数，1–65536 |
| 21 | crc32 | uint32 | 完整 JSON 的 IEEE CRC-32 |
| 25 | fragment | 字节 | 除最后片外均为 103 字节 |

消息按来源地址及端口、会话号、消息号隔离重组。分片可以乱序；缺片、
CRC 不符、冲突重复片、长度不符或非法 JSON 不会交付业务层。

## 消息内容

完整消息是 `{"kind": "...", "data": ...}`，支持 `heartbeat`、`track`、
`track_batch`、`plan_meta`。心跳和规划元信息的 data 为对象。

航迹使用 11 元素数组：

```text
[track_id, category, latitude_e7, longitude_e7, altitude_dm,
 east_velocity_cms, north_velocity_cms, confidence_1000, sensor_id,
 source, time_usec]
```

前十项必须为 int32 范围内的有限整数。机号、类别和传感器号非负；
纬度、经度分别限定在 ±90°、±180°；置信度限定在 0–1000。
高度 -1 表示未知，其他负高度保留。时间戳是 0–2^53−1 的整数微秒值。
批量航迹的 data 为 `{"t": time_usec, "tracks": [...]}`。
Python 服务通过 `Encoder` 自动拆分超过 64 KiB 的航迹批次，每批使用独立消息号。

## 资源和在线状态

- 最多同时重组 64 条消息，每来源最多 8 条；JSON 缓冲合计最多 4 MiB，
  另有有界的分片位图和容器开销。
- 从首片起 2 秒后丢弃未完成消息，重复片不会延长截止时间。
- 完成记录保留 2 秒、最多 512 条，用于短期去重；这不是长期防重放机制。
- UDP 桥最多接纳 8 个来源。单轮读取最多 128 帧或约 2 毫秒，随后交还事件循环。
- 心跳超过 3 秒未更新，QML 在线属性会主动变为离线；停止监听会清空统计和重组状态。
- 融合关联使用来源、会话、传感器和航迹标识；同一来源轨迹的旧时间戳不会刷新目标。
- TTL、控制租约及任务持续时间使用单调时钟。跨进程时间戳仅用于量测和延迟显示，
  未来时间戳的网络延迟显示为未知。

默认监听 `127.0.0.1:15600`。远程部署须配置 QSettings 的
`Swarm/perceptionBindAddress` 和非空 `Swarm/perceptionAllowedSources` 地址白名单。
CRC 和来源白名单不提供加密身份认证，应在可信网络边界内使用。

## 验证入口

共享字节样本位于 `swarm_ext/tests/cpp/protocol_v2_vectors.json`。
C++ `tst_protocol` 与 Python `test_protocol.py` 均读取这些固定样本，
覆盖乱序、缺片、跨来源混片、重复片、CRC 错误、旧版本和资源上限。
`COP` 测试还通过真实 QML 绑定验证心跳到期通知。

```bash
python3 -m unittest discover -s swarm_ext/services/perception -p test_protocol.py -v
python3 swarm_ext/tests/run_all.py --profile standard
```

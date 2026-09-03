# LotSpeed 3.10 Enhanced

本版本以 LotSpeed 3.6.4 的 `mux-throughput` 为基础，保留拥塞门控的 ACK 到达速率自适应，将动态目标精确限定在 168～256 Mbps，并为长期复用的 MUX TCP 增加低流量历史重置。

## 安装

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v310.sh | sudo bash
sudo lotspeed preset mux-throughput
lotspeed status
```

## 主配置

```text
lotserver_rate=32000000          # 256 Mbps 上限
lotserver_min_rate=21000000      # 168 Mbps 下限
lotserver_gain=30                # 3.0x
lotserver_beta=871               # 拥塞时保留约 85%
lotserver_min_cwnd=32
lotserver_max_cwnd=10000
lotserver_adaptive=1
lotserver_pacing_gain=105
lotserver_min_flight_ms=250
lotserver_loss_congest_pct=30
lotserver_loss_recover_pct=25
lotserver_rtt_confirm_samples=20
lotserver_loss_guard=1
lotserver_noncong_beta=1000
lotserver_hd_enable=0
```

`adaptive=0` 时固定使用 256 Mbps 目标。`adaptive=1` 时只在确认拥塞的 `AVOIDING` 状态中动态调整，其余状态立即恢复 256 Mbps。

## 动态调速

每个 TCP 连接独立统计每个 RTT 轮次的 ACK 到达速率。实测速率上升时吸收 25% 新样本，下降时只吸收 12.5% 新样本，减少短时波动。

确认拥塞后：

```text
动态目标 = clamp(平滑 ACK 到达速率 × 1.05,
                 168 Mbps,
                 256 Mbps)
```

触发条件沿用 3.6.4 `mux-throughput`：

```text
丢包 EWMA >= 30%
或
RTT 持续膨胀 20 个轮次，且丢包 EWMA >= 25%
```

拥塞分类解除并经过至少 250ms 后，返回固定 256 Mbps 目标。稳定路径 pacing 为 268.8 Mbps，Jitter 状态最高约 263.7 Mbps，拥塞状态的 pacing 跟随 168～256 Mbps 动态目标。

## MUX 低流量重置

内核每 5 秒检查每条 TCP 的实际发送量。以 256 Mbps 主配置计算，低流量门槛是上限的 10%，即 25.6 Mbps。

连续两个窗口，也就是 10 秒低于该门槛后，清除：

- 旧的 ACK 到达速率估计
- 丢包 EWMA 与 RTT 拥塞计数
- `AVOIDING` 状态与动态目标

目标立即恢复 256 Mbps。后续同一条 MUX TCP 重新出现大流量时，使用新的 ACK 样本重新学习，不继承上一次弱网传输的低速结果。完全空闲超过 10 秒后重新发送也会立即重置。

## 速率统计

```bash
sudo lotspeed rate-status
```

该命令只扫描当前网络命名空间的已建立 LotSpeed 连接，按 pacing rate 统计 `FULL`、`FULL/JITTERY` 和 `ADAPTIVE_168_256M`。`guard-status` 保留为兼容别名。不会启动后台任务或内核日志。

## MUX 内存配置

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

生产环境不建议在大量连接下开启 `lotserver_verbose=1`。

Secure Boot 开启时如果出现 `Key was rejected by service`，需要关闭 Secure Boot，或用已加入 MOK 的密钥签名 `lotspeed.ko`。

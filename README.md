# LotSpeed 3.10.3 Enhanced

本版本以 LotSpeed 3.6.4 的 `mux-throughput` 为基础，保留拥塞门控的 ACK 到达速率自适应，将动态下限设为上限的 60%，并为长期复用的 MUX TCP 增加低流量历史重置。正常 pacing 为 120%，Jitter 为 110%；严重拥塞或持续轻中度丢包进入 adapt 后使用 100%。

## 安装

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v3103.sh | sudo bash
sudo lotspeed preset mux-throughput
lotspeed status
```

## 主配置

```text
lotserver_rate=32000000          # 256 Mbps 上限
lotserver_min_rate_pct=60        # 下限为 rate 的 60%
lotserver_gain=30                # 3.0x
lotserver_beta=871               # 拥塞时保留约 85%
lotserver_min_cwnd=32
lotserver_max_cwnd=10000
lotserver_adaptive=1
lotserver_pacing_gain=120
lotserver_min_flight_ms=250
lotserver_rtt_tolerance_pct=120
lotserver_loss_congest_pct=40
lotserver_loss_recover_pct=30
lotserver_loss_adapt_pct=8
lotserver_rtt_confirm_samples=40
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
                 lotserver_rate × 60%,
                 256 Mbps)
```

只有满足以下条件的 ACK 采样才参与拥塞分类：

- 非 `app_limited`
- 当前采样至少确认 32 个 delivered packets
- 采样区间不超过 2 秒

在合格采样上，满足任一条件即确认拥塞：

```text
丢包 EWMA >= 40%
或
RTT 相对基线膨胀超过 120%，持续 40 个轮次，且丢包 EWMA >= 30%
或
丢包 EWMA 和当前样本均 >= 8%，连续累计 8 个合格轮次
```

中度丢包证据在干净样本出现时快速衰减，所以偶发丢包不会直接触发。`PATH_STABLE` 使用 120% pacing，`PATH_JITTERY` 最多使用 110% pacing，确认拥塞时使用 100% pacing。单次 `TCP_CA_Loss` 或 RTO 仍执行 Linux TCP 的 CWND 退避，但不会单独触发目标速率 adapt。拥塞分类解除并经过至少 250ms 后，返回固定 256 Mbps 目标。默认主配置的动态范围是 153.6～256 Mbps；如果把上限设置为 416 Mbps，下限会自动变为 249.6 Mbps。

`lotserver_loss_adapt_pct` 可以在线调整。调低会让更多持续丢包连接进入 adapt，调高则更严格；它按连接质量工作，不会强制凑出固定比例：

```bash
sudo lotspeed set lotserver_loss_adapt_pct 6   # 更积极
sudo lotspeed set lotserver_loss_adapt_pct 10  # 更保守
```

## MUX 低流量重置

内核每 5 秒检查每条 TCP 的实际发送量。以 256 Mbps 主配置计算，低流量门槛是上限的 10%，即 25.6 Mbps。

连续两个窗口，也就是 10 秒低于该门槛后，清除：

- 旧的 ACK 到达速率估计
- 丢包 EWMA、中度丢包证据与 RTT 拥塞计数
- `AVOIDING` 状态与动态目标

目标立即恢复 256 Mbps。后续同一条 MUX TCP 重新出现大流量时，使用新的 ACK 样本重新学习，不继承上一次弱网传输的低速结果。完全空闲超过 10 秒后重新发送也会立即重置。

## 速率统计

```bash
sudo lotspeed rate-status
```

该命令只扫描当前网络命名空间的已建立 LotSpeed 连接。最近 10 秒发送过数据的连接分别统计为 `ACTIVE_FULL_120`、`ACTIVE_JITTERY_110` 和 `ACTIVE_ADAPTIVE_100`；空闲或停滞连接及其旧 adaptive pacing 单独列出。`guard-status` 保留为兼容别名。

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

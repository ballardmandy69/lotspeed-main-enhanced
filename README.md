# LotSpeed 3.9.1 Enhanced

本版本面向“每位用户独立一条长连接 MUX TCP”的海外服务器回国流量。健康连接保留 upstream `main` 的固定速率表现；只有持续高速发送、效率低于 50% 且确实出现重传的单条 TCP，才会逐级降低目标上限。

## 安装

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v391.sh | sudo bash
sudo lotspeed preset main-guarded
lotspeed status
```

`wan-enhanced`、`domestic-mixed` 和 `mux-throughput` 均为 `main-guarded` 的兼容别名。

## 主配置

```text
lotserver_rate=32000000       # 256 Mbps
lotserver_gain=30             # 3.0x
lotserver_beta=820            # 约保留 80%
lotserver_min_cwnd=32
lotserver_max_cwnd=6000
lotserver_adaptive=1
lotserver_pacing_gain=120
lotserver_loss_guard=0
lotserver_hd_enable=0
```

`adaptive=0` 完全关闭效率保护，固定使用 `lotserver_rate`。`adaptive=1` 只启用以下每连接保护，不会按带宽估计持续改变健康连接。

## 3.9.1 状态机

```text
FULL             256 Mbps，pacing 120%
LIMIT_75         192 Mbps，pacing 100%
LIMIT_DYNAMIC    冻结在 100～192 Mbps，pacing 100%
PROBE_75         探测 192 Mbps，pacing 100%
PROBE_FULL       探测 256 Mbps，pacing 120%
```

内核按每条 TCP 分别统计：

```text
实际发送 = TCP 发送字节（包含重传）
有效接收 = 对端新确认的 ACK 字节
发送效率 = 有效接收 / 实际发送
```

新连接必须先在 `FULL` 完整运行 8 秒，观察期内绝不降档。下降还必须同时满足：

- 窗口平均发送量达到当前目标的 70%
- 发送量不少于 256 KiB，并出现至少 16 个重传段
- 发送效率低于 50%
- 当前不是应用供数不足，也不是明显的对端接收窗口限制

第一次命中只从 `FULL` 降到 `LIMIT_75`。再经过一个完整 5 秒窗口仍满足相同条件，才进入 `LIMIT_DYNAMIC`。动态目标只在进入该状态时计算一次：

```text
clamp(最近 5 秒 ACK 有效均速 × 2.0, 100 Mbps, 192 Mbps)
```

该目标随后冻结。相较 3.9 的 1.5 倍，2.0 倍会给弱网保留更多发送余量：例如 ACK 有效均速为 80 Mbps 时，冻结目标为 160 Mbps，而不是 120 Mbps。限速造成 delivery rate 下降时不会再次递归降低，因此 3.8.4 中可能出现的“限速 -> 测得更低 -> 再限速”反馈环已经删除。对 256 Mbps 主配置，弱网硬地板为 100 Mbps。

## 快速恢复

受限状态每 5 秒重新评估一次。效率达到 80%、重传字节不超过发送字节的 20%，并且连接持续满载时，才向上探测一级：

```text
LIMIT_DYNAMIC -> PROBE_75 -> LIMIT_75
LIMIT_75      -> PROBE_FULL -> FULL
```

每次探测持续约 2 秒，并要求 ACK 有效速率至少达到探测前的目标。成功只升一级；失败立即回到原来的冻结档，经过下一个 5 秒窗口后可再次尝试。连续空闲 30 秒后清除旧证据，下次发送从 `FULL` 开始。

RTT、Jitter、单次丢包和 IP 地址本身都不会直接触发降档，也不会影响同一 IP 的其他 TCP 连接。

## MUX 内存配置

主预设同时写入：

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

这些值限制自动调优缓冲区和本机未发送队列，但不是所有 TCP 内存的硬上限。

## 常用命令

```bash
lotspeed status
sudo lotspeed set lotserver_verbose 1
sudo lotspeed logs 100
sudo lotspeed preset main-guarded
```

如果 Secure Boot 开启并出现 `Key was rejected by service`，需要关闭 Secure Boot，或者使用已加入 MOK 的密钥签名 `lotspeed.ko`。这与拥塞控制算法本身无关。

模块支持 Linux 4.9 及以上内核；Linux 4.19 及以上使用 TCP 字节计数，较旧内核使用数据段计数近似发送载荷。

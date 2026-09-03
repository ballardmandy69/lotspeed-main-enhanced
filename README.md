# LotSpeed 3.9.3 Enhanced

本版本面向“每位用户独立一条长期复用 MUX TCP”的海外服务器回国流量。健康连接和短流保留 upstream `main` 的固定速率行为；只有持续高速至少 30 秒、并连续出现严重重传浪费的单条长连接，才会降低目标上限。

## 安装

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v393.sh | sudo bash
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

`adaptive=0` 完全关闭额外保护并固定使用 `lotserver_rate`。`adaptive=1` 只启用下面的每连接重传保护，不会用 ACK 到达速率持续改变健康连接。

## 判断指标

Linux 4.19 及以上直接读取每条 TCP 的发送与重传字节计数：

```text
原始发送字节 = Δbytes_sent - Δbytes_retrans
重传开销     = Δbytes_retrans / 原始发送字节
流量比例     = Δbytes_sent / 原始发送字节
             = 1 + 重传开销
```

因此：

```text
流量比例 1.3 = 重传开销 30%
流量比例 1.8 = 重传开销 80%
```

保护逻辑不使用 ACK 到达速率、Ping、RTT、Jitter 或 IP 地址作降档依据。每条 TCP 独立判断，同一 IP 的其他连接不会连坐。

## 3.9.3 状态机

```text
FULL             256 Mbps，pacing 120%
LIMIT_75         192 Mbps，pacing 100%
LIMIT_100M       100 Mbps，pacing 100%
PROBE_75         探测 192 Mbps，pacing 100%
PROBE_FULL       探测 256 Mbps，pacing 120%
```

统计窗口固定为 5 秒。新连接必须连续六个窗口达到当前目标的 10% 发送负载，才获得长期高速连接资格；前 30 秒不会受到额外限制。在默认档位下，参与检测的门槛分别为 25.6、19.2 和 10 Mbps。

首次下降要求最近两个 5 秒窗口都满足：

- 发送量不少于 256 KiB，并达到当前目标的 10%
- `总发送/原始发送 >= 1.8`

第 30 秒时已经满足上述两个严重窗口即可从 `FULL` 降至 `LIMIT_75`。在 192 Mbps 档再次连续两个严重窗口，才降至 100 Mbps；不会继续下降。

## 快速恢复

受限连接只要一个有效 5 秒窗口达到：

```text
总发送/原始发送 < 1.3
```

就立即向上探测一级。探测运行一个 5 秒窗口，比例仍低于 1.3 才成功；否则返回原档。

当比例连续处于 `1.3～1.8` 时，不永久冻结：连续五个窗口，也就是 25 秒后强制向上探测一次。期间若比例重新达到 1.8，清零 25 秒计时。

```text
LIMIT_100M -> PROBE_75 -> PROBE_FULL -> FULL
LIMIT_75   -> PROBE_FULL -> FULL
```

长期高速资格在连接持续传输时保留。任意两个连续窗口低于当前档位 10% 的负载后，认定长期下载结束，清除档位、计时和资格并恢复 `FULL`。后续重新高速传输需要重新观察 30 秒。小流量窗口不计算 1.3/1.8 比例。

## 无日志统计

生产环境不要在大量连接下开启 `lotserver_verbose=1`。可以按需执行：

```bash
sudo lotspeed guard-status
```

该命令只扫描执行时所在网络命名空间中的已建立连接，并按 pacing rate 汇总 `FULL`、192 Mbps 和 100 Mbps 档位；不会启动后台任务或写入内核日志。探测档与对应速率档显示在同一组中。

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

如果 Secure Boot 开启并出现 `Key was rejected by service`，需要关闭 Secure Boot，或者使用已加入 MOK 的密钥签名 `lotspeed.ko`。这与拥塞控制算法本身无关。

模块支持 Linux 4.9 及以上内核；Linux 4.19 及以上使用 TCP 字节计数，较旧内核使用数据段计数近似发送载荷。

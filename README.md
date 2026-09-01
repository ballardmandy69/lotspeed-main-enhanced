# LotSpeed 3.8.4 Enhanced

本版本面向“每位用户独立一条长连接 MUX TCP”的海外服务器回国流量。正常连接恢复到经过验证的 upstream `main` 固定速率行为，只对持续发送效率很低的单条 MUX 降低目标速率上限。

## 安装

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v384.sh | sudo bash
sudo lotspeed preset main-guarded
lotspeed status
```

`wan-enhanced`、`domestic-mixed` 和 `mux-throughput` 在 3.8.4 中均为该主预设的兼容别名。

## 主配置

```text
lotserver_rate=32000000       # 256 Mbps
lotserver_gain=30             # 3.0x
lotserver_beta=820            # 约保留80%
lotserver_min_cwnd=32
lotserver_max_cwnd=6000
lotserver_adaptive=1
lotserver_pacing_gain=120
lotserver_loss_guard=0
lotserver_hd_enable=0
```

3.8.4 中 `lotserver_adaptive` 的语义已经收窄：

```text
adaptive=0：固定使用 lotserver_rate，不做低效率分档
adaptive=1：健康流仍固定使用 lotserver_rate，仅启用每连接效率保护
```

RTT、Jitter 或单次丢包不会直接触发效率分档。

## 四档目标上限

内核按每条 TCP 分别统计：

```text
实际发送 = 新发送TCP载荷 + 重传TCP载荷
有效接收 = 对端新确认的ACK载荷
发送效率 = 有效接收 / 实际发送
```

普通降档需要持续发送达到当前目标至少 70%、并且没有被对端接收窗口限制的完整十秒窗口。若效率低于70%，并且已经满足发送量条件，3.8.4 会在约两秒后进入受限档：效率在50%～70%时进入70%目标档，30%～49%进入50%状态档，低于30%进入30%严重状态档。低于50%的两个状态都使用“平滑有效速率 × 1.5”的动态目标上限，并且不超过 `lotserver_rate`。至少发送256 KB且出现16个重传段的明显异常流可以覆盖普通接收窗口过滤。70%～79%的中等效率仍按完整十秒窗口判断。

| 连续十秒发送效率 | 每连接目标上限 | 256 Mbps 配置下 |
| --- | ---: | ---: |
| `>= 80%` | `100% rate` | 256 Mbps |
| `50%～79%` | `70% rate` | 179.2 Mbps |
| `30%～49%` | `min(100% rate, 1.5×平滑有效速率)` | 例如有效速率102.4 Mbps时为153.6 Mbps |
| `< 30%` | `min(100% rate, 1.5×平滑有效速率)` | 例如有效速率51.2 Mbps时为76.8 Mbps |

分档只修改该连接的 `target_rate` 上限。平滑值在有效速率下降时立即跟随，恢复时按旧值75%与新采样25%加权上升，避免单次 ACK 突发把目标速率突然拉高。`gain=30`、`beta=820`、`cwnd=32..6000` 和 `pacing_gain=120%` 保持原版配置。

目标速率不是严格整形。例如目标速率为有效速率的1.5倍时，配合 `pacing_gain=120%`，内部 pacing 上限约为有效速率的1.8倍；这里的1.5倍指 Lotspeed 的目标速率上限。

## 降档与恢复

```text
FULL -> LIMIT_70：连续十秒效率为50%～79%，或合格的两秒窗口效率为50%～70%
FULL -> LIMIT_50：合格窗口效率为30%～49%
FULL -> LIMIT_30：合格窗口效率低于30%（极端快速保护）
LIMIT_70 -> LIMIT_50：连续十秒效率低于50%
LIMIT_70/LIMIT_50 -> LIMIT_30：两秒窗口效率低于30%
```

恢复采用两秒一级的受控探测：

```text
LIMIT_30 效率连续两秒 >=85% -> 探测 LIMIT_50
LIMIT_50 效率连续两秒 >=85% -> 探测 LIMIT_70
LIMIT_70 效率连续两秒 >=85% -> 探测 FULL
```

探测持续两秒并逐级恢复。达到 80% 保持全速，50%～79%保持70%档；低于50%时仍按平滑有效速率的1.5倍运行。探测失败后冷却十秒，避免弱线路在动态目标和256 Mbps之间不断振荡。

短暂的 MUX 发送停顿保留效率证据，避免每次突发都重新开始观察。持续30秒没有有效负载后才清除旧档位和平滑速率估计，下次传输从FULL开始重新观察；MUX keepalive小包不会形成有效降档窗口。

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

`tcp_notsent_lowat` 会对应用未发送队列施加背压，但不是单连接总内存硬上限。已经发送但尚未确认的数据、重传队列和 TCP 元数据仍会占用发送缓存。

## 常用命令

```bash
lotspeed status
sudo lotspeed set lotserver_verbose 1
sudo lotspeed logs 100
sudo lotspeed preset main-guarded
```

旧版本升级时，安装器会清理包含已删除 `degraded_*` 参数的旧 `/etc/modprobe.d/lotspeed.conf`，避免新模块因未知参数加载失败。

如果 Secure Boot 开启并出现 `Key was rejected by service`，需要关闭 Secure Boot，或者使用已加入 MOK 的密钥签名 `lotspeed.ko`。这与拥塞控制算法本身无关。

## 构建

```bash
git clone https://github.com/ballardmandy69/lotspeed-main-enhanced.git
cd lotspeed-main-enhanced
make
```

模块支持 Linux 4.9 及以上内核；Linux 4.19 及以上使用精确 TCP 字节计数，较旧内核使用数据段计数近似实际发送载荷。

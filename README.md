# LotSpeed 3.10.11 Enhanced

基于 3.10.10 调整中度 adapt 入口：用当前合格窗口的新增丢包指标确认，不再等待 EWMA 先达到中度门槛。不按 IP 或连接数量配额限速，不恢复 3.8 的到达率分档。每个底层 TCP 独立判断；AnyTLS 的复用不意味着一个 IP 或用户只有一条 TCP。

## 安装与升级

以 root 运行：

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v31011.sh | bash
lotspeed status
lotspeed rate-status
```

升级已加载的旧模块时，安装器保留新版本支持的运行中模块参数，包括自定义 rate、gain、min_rate_pct 和确认次数，并写入模块配置。不要再运行 preset，除非确实要覆盖自定义值。旧模块没有加载时，不会从旧配置文件自动迁移参数。

旧模块仍被 TCP 或网络命名空间引用时，安装器会停止并恢复原默认拥塞算法，不会强杀连接或强制卸载。需要先安排业务连接退出，再重试。Secure Boot 拒绝未签名模块时，需要使用已受信任的密钥签名，或调整 Secure Boot 设置。

首次安装使用模块默认值；需要应用主预设及配套缓冲区配置时运行：

```bash
lotspeed preset mux-throughput
```

## 与 3.10.10 的区别

| 部分 | 3.10.10 | 3.10.11 |
| --- | --- | --- |
| 中度门槛 | EWMA 达标且本窗口有新增丢包 | 本窗口新增丢包指标达标即可计数，不等待 EWMA |
| 中度样本量 | 合格丢包窗口至少 1 个 delivered 包 | 中度加计数还要求本窗口至少 8 个 delivered 包 |
| 确认次数 | 默认 5，可运行时调整 | 不变；设为 2 时两个合格丢包窗口可触发 |
| 严重入口和退出 | 严重 EWMA/RTT 入口、EWMA 衰减恢复 | 不变 |
| 丢包证据与空闲复用 | 独立计数、2 秒过期、排空后约 10 秒重置 | 不变 |

保持默认参数、动态目标计算、CWND、pacing、RTT 门槛和快速退出逻辑不变。没有新增调参项、定时器、逐包日志或动态内存分配，私有状态仍在 88 字节内。

## 默认参数

```text
lotserver_rate=45000000          # 360 Mbps 目标上限
lotserver_min_rate_pct=50        # 自适应目标下限 180 Mbps
lotserver_gain=26
lotserver_beta=871
lotserver_min_cwnd=32
lotserver_max_cwnd=10000
lotserver_adaptive=1
lotserver_pacing_gain=120
lotserver_min_flight_ms=250
lotserver_rtt_tolerance_pct=80
lotserver_loss_congest_pct=30
lotserver_loss_recover_pct=25
lotserver_loss_adapt_pct=3
lotserver_loss_adapt_samples=5
lotserver_rtt_confirm_samples=20
lotserver_loss_guard=1
lotserver_noncong_beta=1000
lotserver_hd_enable=0
lotserver_verbose=0
```

## 判断与恢复

每个 packet-timed RTT 边界尝试消费独立丢包窗口。至少有 1 个新 delivered 包，且经过了至少一个时钟 tick、窗口不超过 2 秒，才更新 EWMA。零时长或没有 delivered 的窗口暂存；超过 2 秒的窗口作废，不会当作健康样本或继续增加异常计数。短暂 TX_START 不会清空这组计数。

```text
本次丢包指标 = 新增标记丢包数 / (新增 delivered 数 + 新增标记丢包数)
EWMA = 旧值 + 约 1/8 × (本次指标 - 旧值)
```

该指标不是线路真实丢包概率，也不是 bytes_retrans 占比。新增标记丢包和实际重传次数不同。delivered 是内核 ACK/SACK 的包计数，不等于远端应用的实时接收字节。

中度加计数要求本窗口至少 8 个 delivered 包、有新增标记丢包，且本次丢包指标达到 loss_adapt_pct（默认 3%，内部按 1/1024 整数量化）。不再要求 EWMA 先达标；默认累积到 5 次走中度 adapt。未满足加计数条件且 EWMA 降到约 2.2% 以下时，每个合格窗口扣除两个确认。无新增丢包但旧 EWMA 仍高时只保留计数，不凭旧证据增加计数。确认是带衰减的计数，不是严格连续次数，也不是秒数。

运行中已设为 1 的值会被升级保留，新入口下可能由一个合格突发触发。需要保留两次确认时运行 lotspeed set lotserver_loss_adapt_samples 2。至少 8 包仅保护中度入口的小样本；严重 EWMA 入口仍按原规则判断。此改动不保证固定比例的连接进入 adapt。

原有严重入口仍保留：EWMA 达到 30%，或 RTT 膨胀超过基线的 80% 加抖动余量、累计 20 个合格 RTT 轮次且 EWMA 达到 25%。RTT 学习仍要求至少 8 个 delivered 包、采样不超过 2 秒。

丢包和 RTT 学习接受 app_limited；但保留该标记对低速带宽样本的保护，不因应用暂时没数据就拉低带宽估计。其语义见 [Linux TCP rate sampling](https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_rate.c)。

```text
AVOIDING 中目标 = clamp(平滑 ACK 到达速率 × 1.05, rate × min_rate_pct / 100, rate)
STABLE pacing = 目标 × 120%
JITTERY pacing = 目标 × 110%（主预设下）
CONGESTED pacing = 目标 × 100%
```

速率样本上升吸收 25% 新值，下降吸收 12.5% 新值。分类解除、且本次 AVOIDING 状态持续超过 250ms 后退出；不是要求健康持续 250ms。默认目标范围 180～360 Mbps，稳定 pacing 432 Mbps；这些都不是实际 goodput 保证。

## AnyTLS 空闲重置

仅速度低不再触发历史重置。待发送或未确认的数据都会阻止空闲判定，包括接收窗口关闭、RTO 或慢速下载；这些现象本身也不单独增加丢包证据。

ACK 回调观察到队列排空后开始空闲计时。约 10 秒后在后续回调或真正重新发送时清除旧 ACK 速率、丢包和 RTT 拥塞证据，恢复完整目标。新的写入会结束空闲计时；间隔短于 10 秒的小突发不保证完整重置，但仍按新的合格样本更新和衰减。完全没有回调时不在后台计时唤醒，空闲 socket 的 pacing 显示可能暂留旧值。

## 统计和验证

lotspeed rate-status 通过 ss 的 pacing 推测状态，并区分最近 10 秒发送过数据和空闲/停滞连接。它不是内部状态读取，也不能把最近发送过的所有连接都当成持续下载。应对异常连接采集同一四元组的多次 ss -tinm，比较增量，不能只追求 adapt 数量多。

python3 tests/run_model_tests.py 编译实际生产函数，测试短窗口累积、单次和持续丢包、app_limited、积压保护、空闲复用、计数器回绕、过期和 1～255 确认设置。CI 在 HZ=100/250/1000 下运行这些逻辑测试并进行内核模块编译；这不是生产网络性能验证。

主预设的缓冲区设置保持：

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

# LotSpeed 3.10.14 Enhanced

基于 3.10.13，修复已判定严重拥塞且仍有积压的连接，在反馈停顿后因丢包历史清零而过早恢复全速的问题。
过期采样仍丢弃，但符合严重条件的积压连接保留拥塞依据；收到健康反馈后仍按原规则恢复。
每个底层 TCP 独立判断，不按 IP 或连接数量配额限速。

## 安装与升级

以 root 安装或升级到固定版本 3.10.14：

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/v3.10.14/install-v31014.sh | bash
lotspeed status
lotspeed rate-status
```

也可从 [发布页](https://github.com/ballardmandy69/lotspeed-main-enhanced/releases/tag/v3.10.14)
下载自解压安装器，以 root 执行 `bash lotspeed-3.10.14-enhanced-installer.run`。

升级已加载的旧模块时，安装器保留新版本支持的运行中模块参数，包括自定义 rate、gain、min_rate_pct 和确认次数，并写入模块配置。不要再运行 preset，除非确实要覆盖自定义值。旧模块没有加载时，不会从旧配置文件自动迁移参数。

旧模块仍被 TCP 或网络命名空间引用时，安装器会停止并恢复原默认拥塞算法，不会强杀连接或强制卸载。需要先安排业务连接退出，再重试。Secure Boot 拒绝未签名模块时，需要使用已受信任的密钥签名，或调整 Secure Boot 设置。

首次安装使用模块默认值；需要应用主预设及配套缓冲区配置时运行：

```bash
lotspeed preset mux-throughput
```

## 与 3.10.13 的区别

| 部分 | 3.10.13 | 3.10.14 |
| --- | --- | --- |
| 超过 2 秒的丢包采样 | 清零丢包 EWMA 和中度确认计数 | 已严重拥塞且仍有积压时保留，否则仍清零 |
| 过期数据与低速学习授权 | 丢弃过期样本，清零 app_limited 严重确认 | 不变，不用旧样本继续下修估速 |
| 健康反馈与排空 MUX | 按既有规则恢复或空闲重置 | 不变，不因等待反馈本身直接解除严重拥塞 |
| 普通/中度连接、adapt 入口 | 原有规则 | 不变 |
| 默认值、速率保底、gain 与升级参数保留 | 原有参数 | 不变，无新增模块参数 |

保留历史仅适用于 adaptive 开启、turbo 关闭、路径已为 CONGESTED、
丢包 EWMA 至少 max(30%, loss_congest_pct)，并且仍有未发送或待确认数据的连接。
此处保留的是拥塞分类依据，不是过期窗口的交付/丢包计数，也不是低速学习的 10 秒授权。
默认参数、目标公式、CWND、pacing 增益和有效样本驱动的恢复不变，私有状态仍为 88 字节；
不增加定时器、逐包日志或动态内存分配。

例如 rate=800 Mbps、min_rate_pct=8 时，每条连接仍有 64 Mbps 目标下限。
本次只修复错误解除限制，不突破保底，也不保证这个下限适合所有严重异常连接。

### 保留的 3.10.13 严重学习规则

放开低速 app_limited 样本仍需有效观察累计至少约 10 秒：adaptive 开启、turbo 关闭、路径分类为 CONGESTED，
丢包 EWMA 至少达到 max(30%, loss_congest_pct)，每个窗口至少交付 8 包且不超过 2 秒，并有新增实际重传。
积压要求为至少 1 MSS 未发送新数据，或至少 8 包且序列跨度至少 8 MSS 的已发送待确认数据。
积压本身不能触发保护，仍必须有持续严重丢包和新增重传的证据。

不合格窗口不计入 10 秒，也不能授权低速估计下修；连续不合格时间累计达到 2 秒、采样过期、
积压不足或离开 CONGESTED 分类时清零。计时仍由回调推进，不是每连接后台定时监控。

计时从第一个合格窗口结束后开始，EWMA 暖机还需要额外时间，所以并非从第一次重传起恰好 10 秒触发。重传计数低 16 位只用于确认发生变化，不计算比例；恰好增加 65536 的整数倍时保守地放弃本次确认。此处 30% 是既有丢包标记指标的 EWMA，不是重传字节占比；不会复用 1% 中度门槛作为严重异常判据。

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

丢包和 RTT 学习接受 app_limited；低速带宽样本默认仍被过滤，仅在上述严重异常条件持续满足时参与下降。ACK 聚合补偿中的过滤不变。其语义见 [Linux TCP rate sampling](https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_rate.c)。

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

python3 tests/run_model_tests.py 编译实际生产函数，测试短窗口累积、单次和持续丢包、普通及严重异常 app_limited 估速、
待确认旧包重传、短间隔暂停、ACK 聚合保护、空闲复用、计数器回绕、过期和 1～255 确认设置。
CI 在 HZ=100/250/1000 下运行这些逻辑测试并进行内核模块编译。
高发送目标、低交付和旧包重传场景为合成测试，并非对现场未观测回调的完整回放；不保证生产吞吐或重传比。

主预设的缓冲区设置保持：

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

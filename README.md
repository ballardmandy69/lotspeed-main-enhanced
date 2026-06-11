### lotspeed 开心版

<div align=center>
    <img src="https://github.com/uk0/lotspeed/blob/main/logo.png" width="400" height="400" />
</div>

### v3.5.2 国内混合终端宽松判定版

海外服务器同时面向国内不同地区、宽带、WiFi、移动网络和校园网时，使用：

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v352.sh | sudo bash
lotspeed preset domestic-mixed
lotspeed status
```

`domestic-mixed` 保留每连接 256 Mbps 上限，但每条 TCP 连接会独立识别稳定、抖动和拥塞状态。默认 `gain=30`，拥塞保留约 85%，自适应速率下限为上限的 60%（当前配置为 153.6 Mbps）。拥塞判定已放宽到约 20% 丢包；RTT 超过“基线 + 60% + 4 倍 Jitter”持续 12 个样本时，还必须同时有至少约 8% 丢包才进入拥塞。单纯高延迟或大 Jitter 不再判定拥塞。

### main-enhanced

本仓库的 `main` 基于 [`uk0/lotspeed`](https://github.com/uk0/lotspeed) 的 `main`，保留固定速率模式的直接性，并选择性合并其他分支中适合公网高延迟、随机丢包线路的设计：

* 修正 adaptive 模式把 `rate_sample.delivered` 包数误当成字节数的问题。
* 修正 Linux 6.10 起 `cong_control` 回调签名的兼容边界。
* ProbeRTT 默认每 30 秒触发 150ms，并保留 50% 原窗口，减少周期性吞吐断崖。
* 无明显 RTT 膨胀时，对随机丢包使用较温和的退让。
* RTT 超过 120ms 时可增加 CWND gain，但不会突破 `lotserver_max_cwnd`。
* 移除不安全的“卸载失败后重新注册算法”流程。

本地测试安装：

```bash
git clone https://github.com/ballardmandy69/lotspeed-main-enhanced.git
cd lotspeed-main-enhanced
sudo bash install.sh
lotspeed preset wan-enhanced
lotspeed status
```

分支推送后可直接安装：

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v352.sh | sudo bash
lotspeed preset domestic-mixed
```

`wan-enhanced` 固化了已验证的 256Mbps 配置：`rate=32000000`、`gain=30`、`beta=820`、`cwnd=32..6000`、`adaptive=0`，并启用高延迟补偿和随机丢包保护。

普通非 CN2 电信 163 回国方向建议：

```bash
lotspeed preset ct-163-return
```

该预设将 `32000000` 作为每连接上限而非固定发送目标，启用 adaptive，pacing 保留5%余量，并对所有丢包进行拥塞退让。

如果旧安装输出中出现 `M=/root`，说明编译误用了 `/root` 下的旧源码。`3.5.2-enhanced` 使用不可变版本整包安装并修复该问题；重新运行一键安装时，正确日志应显示：

```text
make -C /lib/modules/.../build M=/opt/lotspeed modules
```

安装器会在替换旧模块前检查 `.ko` 版本和增强参数，不再接受遗留构建产物。


### branch explanation

* `main`: lotspeed 自动优化版本最新版
* `v3.1`: lotspeed 最小优化算法版本
* `zeta-tcp`: lotspeed zeta-tcp 版本([Appex Networking zeta-tcp](https://appexnetworks.com/wp-content/uploads/2024/02/ZetaTCP-Whitepaper-V2.0.pdf))


* auto install


```bash
# 直接安装最新版
wget -qO- https://raw.githubusercontent.com/uk0/lotspeed/main/install.sh | sudo bash
# zeta-tcp 版本
wget -qO- https://raw.githubusercontent.com/uk0/lotspeed/zeta-tcp/install.sh | sudo bash
# 暴力版本
wget -qO- https://raw.githubusercontent.com/uk0/lotspeed/v3.1/install.sh | sudo bash
```


* manual compile and load

```bash

# 下载代码/编译

git clone https://github.com/uk0/lotspeed.git 

cd lotspeed && make

# 加载模块
sudo insmod lotspeed.ko

# 设置为当前拥塞控制算法
sudo sysctl -w net.ipv4.tcp_congestion_control=lotspeed
sudo sysctl -w net.ipv4.tcp_no_metrics_save=1

# 查看是否生效
sysctl net.ipv4.tcp_congestion_control

# 查看日志
dmesg -w

```

### LotSpeed 核心参数配置说明表

| 参数名称 (`sysctl`/`module`)           | 作用说明 (Description)                                        | 单位/换算 (Unit) | 默认值 | 推荐范围 (Ratio/Range) | 调整建议 |
|:-----------------------------------|:----------------------------------------------------------| :--- | :--- | :--- | :--- |
| **`lotserver_rate`**               | **全局物理带宽上限**<br>控制服务器发包的物理天花板，防止撑爆网卡或被运营商QoS。             | **Bytes/sec**<br>100Mbps ≈ 12,500,000 | 125000000<br>(1Gbps) | **物理带宽的 90% - 95%** | **必填项**。设为你的 VPS 物理端口带宽上限（如 100M 口设为 `11500000`）。不要设得比物理带宽大。 |
| **`lotserver_start_rate`**         | **zeta-tcp版本独有，软启动初始速率**<br>新连接建立时的起步速度。保护小带宽客户端不被瞬间流量淹没。 | **Bytes/sec**<br>10Mbps ≈ 1,250,000 | 6250000<br>(50Mbps) | **物理带宽的 30% - 50%** | 对于 100M 口，建议设为 `5000000` (40Mbps) 到 `7500000` (60Mbps)。设太高会导致起步丢包，设太低起步慢。 |
| **`lotserver_gain`**               | **拥塞窗口增益 (Pacing Gain)**<br>倍率因子。决定算法有多“激进”地去抢占带宽。        | **数值 / 10**<br>30 = 3.0倍 | 30 | **15 (1.5倍) - 30 (3.0倍)** | 当前国内混合终端配置默认使用 `30`，优先维持足够在途数据。 |
| **`lotserver_beta`**               | **丢包退让比例 (Fairness)**<br>当发生严重拥塞必须降速时，保留多少窗口。             | **数值 / 1024**<br>871 ≈ 保留85% | 871 | **614 (60%) - 921 (90%)** | 当前默认 `871`，发生拥塞时保留约 85.06% 的窗口。 |
| **`lotserver_min_cwnd`**           | **最小拥塞窗口**<br>无论网络多差，窗口绝不低于此值。                            | **Packets (包数)** | 16 | **4 - 64** | 16 是安全值。设为 `32` 或 `64` 可以提高起步速度，但在拥塞时可能加剧丢包。 |
| **`lotserver_max_cwnd`**           | **最大拥塞窗口**<br>窗口的绝对物理上限，防止 Bufferbloat。                   | **Packets (包数)** | 15000 | **5000 - 30000** | 100Mbps 建议 `5000-8000`。<br>1Gbps 建议 `15000-25000`。<br>设太大无意义，会占用内存。 |
| **`lotserver_turbo`**              | **暴力模式 (Turbo)**<br>是否无视所有丢包信号。                           | **0 (关) / 1 (开)** | 0 | **建议 0** | 除非你在进行压力测试，否则不要开。开启后容易被运营商直接断流。 |
| **`lotserver_safe_mode`**          | **zeta-tcp版本独有，安全熔断 (Safe Mode)**<br>是否在丢包率 >15% 时强制介入降速。              | **0 (关) / 1 (开)** | 1 | **建议 1** | 建议始终开启。这是防止 SSH 断连的最后一道防线。 |

### 常用带宽换算表 (Bytes/sec)

| 带宽 (Mbps) | 参数值 (Bytes/s) | 备注 |
| :--- | :--- | :--- |
| 10 Mbps | `1250000` | 适合作为 Start Rate |
| 20 Mbps | `2500000` | |
| 50 Mbps | `6250000` | 默认 Start Rate |
| 100 Mbps | `12500000` | 常见的 VPS 上限 |
| 200 Mbps | `25000000` | |
| 500 Mbps | `62500000` | |
| 1 Gbps | `125000000` | 默认 Global Rate |

### test youtube (v3.1 version) 

>测试前提，服务器1Gbps，客户端100Mbps带宽

<div align=center>
    <img src="https://github.com/uk0/lotspeed/blob/main/v3.1.png" width="1024" height="768" />
</div>


### test youtube (zeta-tcp version)

>测试前提，服务器1Gbps，客户端100Mbps带宽，丢包率 5% ～ 16%

<div align=center>
    <img src="https://github.com/uk0/lotspeed/blob/main/zeta-tcp.png" width="1024" height="768" />
</div>



### changelog
* 已经测试支持 `debian`,`ubunut` 5.x.x ,6.x.x 内核
* 对抗丢包能力提升
* 优化算法细节，提升稳定性


[![Star History Chart](https://api.star-history.com/svg?repos=uk0/lotspeed&type=Date)](https://star-history.com/#uk0/lotspeed&Date)

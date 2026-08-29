# LotSpeed 3.8.3 Enhanced

LotSpeed 3.8.3 returns healthy connections to the fixed-rate behavior of the
upstream `main` profile and adds one internal per-connection safeguard for
long-lived TCP Mux traffic.

## Recommended profile

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v383.sh | sudo bash
sudo lotspeed preset main-guarded
lotspeed status
```

The main profile is:

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `32000000` bytes/s (256 Mbps) |
| `lotserver_gain` | `30` (3.0x) |
| `lotserver_beta` | `820` (about 80% retained) |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `6000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_pacing_gain` | `120` percent |
| `lotserver_loss_guard` | `0` |
| `lotserver_hd_enable` | `0` |

In 3.8.3, `lotserver_adaptive=1` enables only the per-flow efficiency guard.
Healthy flows retain the fixed `lotserver_rate`; they are not continuously
adapted to measured bandwidth. Setting it to `0` disables the guard and gives
the fixed upstream-main behavior.

## Efficiency tiers

The guard compares TCP payload sent on the wire with newly acknowledged
payload. On Linux 4.19 and newer, transmitted payload includes the kernel's
byte counters for original and retransmitted data. Older supported kernels use
a segment-counter approximation.

| Ten-second delivery efficiency | Per-flow target ceiling |
| --- | ---: |
| 80% or higher | 100% of `lotserver_rate` |
| 50% through 79% | 70% of `lotserver_rate` |
| 30% through 49% | 50% of `lotserver_rate` |
| Below 30% | 30% of `lotserver_rate` |

With the recommended 256 Mbps ceiling, the tiers are 256, 179.2, 128, and
76.8 Mbps. The tier changes only the connection's target ceiling. CWND gain,
loss beta, pacing gain, and minimum/maximum CWND remain the same.

The configured target is not a strict shaper. With `pacing_gain=120`, a
76.8 Mbps target permits an internal pacing rate up to 92.16 Mbps.

## Transition rules

Normal downshifts require a complete 10-second active window. The connection
must transmit at least 70% of its current target during that window.
Receive-window-limited samples are discarded for the normal path. A flow below
70% efficiency is fast-braked after a qualifying 2-second sample: 50%-70%
enters the 70% tier, 30%-49% enters the 50% tier, and below 30% enters the 30%
tier. The severe path can override the normal receive-window filter only when
the sample contains at least 256 KB of transmitted data and 16 retransmitted
segments. The 70%-79% middle range still uses the complete 10-second window.

Recovery is deliberately faster:

1. A limited tier must reach at least 85% efficiency for two seconds.
2. The guard probes one tier higher for two seconds: 30%, 50%, 70%, then full.
3. At least 80% efficiency keeps the full-rate probe; 50%-79% keeps the 70%
   tier, 30%-49% keeps the 50% tier, and below 30% returns to the 30% tier.
4. A failed probe waits 10 seconds before another upward probe.
5. Short Mux pauses preserve evidence; only 30 seconds of idle time resets the
   next active period to the full tier with fresh counters.

These constants are internal by design. Version 3.8.3 does not expose another
set of degraded-mode tuning parameters.

## Mux socket buffers

`main-guarded`, `wan-enhanced`, `domestic-mixed`, and `mux-throughput` apply:

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

These values bound autotuned socket buffers and local TCP output backlog. They
do not cap sent-but-unacknowledged flight data or total memory for all sockets.

## Validation

```bash
iperf3 -c SERVER -P 1 -t 60
ss -tin
lotspeed status
sudo lotspeed logs 100
```

Tier transitions are logged only when `lotserver_verbose=1`.

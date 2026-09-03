# LotSpeed 3.9.3 Enhanced

LotSpeed 3.9.3 keeps the upstream `main` fixed-rate behavior for healthy and
short-lived TCP Mux connections. Its per-connection guard limits only sustained,
high-rate flows with severe retransmission overhead.

## Recommended profile

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v393.sh | sudo bash
sudo lotspeed preset main-guarded
lotspeed status
```

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

Setting `lotserver_adaptive=0` disables the retransmission guard. No new module
parameters are introduced.

## Signal and states

On Linux 4.19 and newer the guard calculates five-second deltas:

```text
original bytes       = bytes_sent - bytes_retrans
retransmit overhead  = bytes_retrans / original bytes
wire ratio           = bytes_sent / original bytes
                     = 1 + retransmit overhead
```

It does not use ACK arrival rate, Ping, RTT, jitter, or the client IP to select
a guard tier.

| State | Target with the main profile | Pacing gain |
| --- | ---: | ---: |
| `FULL` | 256 Mbps | 120% |
| `LIMIT_75` | 192 Mbps | 100% |
| `LIMIT_100M` | 100 Mbps | 100% |
| `PROBE_75` | 192 Mbps | 100% |
| `PROBE_FULL` | 256 Mbps | 120% |

## Qualification and limiting

A flow must transmit at least 10% of its current target for six consecutive
five-second windows before it is eligible. This gives every new transfer 30
seconds at the original full rate. With the default profile, the activity
thresholds are 25.6, 19.2, and 10 Mbps in the three respective tiers.

An eligible flow moves from `FULL` to `LIMIT_75` only after two consecutive
active windows have a wire ratio of at least 1.8. Two more consecutive severe
windows at 192 Mbps are required before moving to `LIMIT_100M`. The target never
drops below 100 Mbps with the main profile.

## Recovery and Mux reuse

One active window below a 1.3 wire ratio starts a one-tier upward probe. Each
probe lasts five seconds and succeeds only if its own ratio remains below 1.3.

A limited flow continuously between 1.3 and 1.8 is not frozen forever. After
five such windows (25 seconds), it receives a forced upward probe. A severe
window resets that timer.

Two consecutive low-activity windows clear the tier, timers, and long-flow
qualification. The next high-rate transfer on the reused Mux starts at `FULL`
and receives a fresh 30-second observation period. Ratios from low-volume
windows are ignored.

## On-demand visibility

Do not enable verbose kernel logging on a high-connection-count production
host. Use this on-demand snapshot instead:

```bash
sudo lotspeed guard-status
```

It scans established LotSpeed sockets in the current network namespace and
groups them by pacing rate. It does not create a daemon or write kernel logs.

## Mux socket buffers

The guarded main aliases apply:

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

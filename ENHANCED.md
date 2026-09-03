# LotSpeed 3.10 Enhanced

LotSpeed 3.10 restores the congestion-gated adaptive model from the 3.6.4
`mux-throughput` profile. It adds an exact 168 Mbps floor and clears stale
per-connection learning after sustained low Mux traffic.

## Install

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v310.sh | sudo bash
sudo lotspeed preset mux-throughput
lotspeed status
```

## Main profile

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `32000000` bytes/s (256 Mbps) |
| `lotserver_min_rate` | `21000000` bytes/s (168 Mbps) |
| `lotserver_gain` | `30` (3.0x) |
| `lotserver_beta` | `871` (about 85% retained) |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `10000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_pacing_gain` | `105` percent |
| `lotserver_min_flight_ms` | `250` ms |
| `lotserver_loss_congest_pct` | `30` percent |
| `lotserver_loss_recover_pct` | `25` percent |
| `lotserver_rtt_confirm_samples` | `20` rounds |
| `lotserver_loss_guard` | `1` |
| `lotserver_noncong_beta` | `1000` |
| `lotserver_hd_enable` | `0` |

## Dynamic rate

Outside congestion avoidance, the target remains 256 Mbps. During confirmed
congestion it becomes:

```text
clamp(smoothed ACK arrival rate * 1.05, 168 Mbps, 256 Mbps)
```

The arrival estimate absorbs 25% of a higher sample and 12.5% of a lower
sample. Congestion detection follows the 3.6.4 Mux profile: 30% loss EWMA, or
20 sustained RTT-inflated rounds with at least 25% loss EWMA. Once congestion
clears for 250 ms, the target returns to 256 Mbps.

## Low-traffic reset

Each TCP connection has a five-second activity window. Two consecutive windows
below 10% of the configured ceiling clear the ACK-rate estimate, loss EWMA,
RTT-congestion evidence, avoidance state, and adaptive target. With the main
profile the threshold is 25.6 Mbps and the reset takes 10 seconds. A truly idle
connection is also reset when transmission restarts after 10 seconds.

The reset is per TCP connection and does not affect other clients or use an IP
address. It prevents a reused Mux from carrying a stale low-rate result into a
later transfer.

## Visibility

```bash
sudo lotspeed rate-status
```

This on-demand command groups established LotSpeed sockets in the current
network namespace as `FULL`, `FULL/JITTERY`, `ADAPTIVE_168_256M`, or unknown.
`guard-status` remains as a compatibility alias. No daemon or kernel logging is
enabled.

## Mux buffers

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

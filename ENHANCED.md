# LotSpeed 3.7.1 Enhanced

This branch is a speed-first performance update on top of `main`.

## Recommended profile

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v371.sh | sudo bash
lotspeed preset wan-enhanced
lotspeed status
```

The recommended preset gives healthy and moderately congested TCP connections
a 250 Mbps ceiling. Only sustained severe loss enters the 50-100 Mbps range:

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `31250000` bytes/s (250 Mbps) |
| `lotserver_gain` | `30` |
| `lotserver_beta` | `820` |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `10000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_congestion_only` | `0` |
| `lotserver_pacing_gain` | `100` percent |
| `lotserver_min_rate_pct` | `50` percent (125 Mbps normal floor) |
| `lotserver_min_flight_ms` | `0` (disabled) |
| `lotserver_avoid_hold_ms` | `1000` milliseconds |
| `lotserver_loss_congest_pct` | `20` percent |
| `lotserver_loss_recover_pct` | `8` percent |
| `lotserver_rtt_confirm_samples` | `8` |
| `lotserver_rtt_tolerance_pct` | `60` percent |
| `lotserver_loss_guard` | `1` |
| `lotserver_noncong_beta` | `900` |
| `lotserver_hd_enable` | `0` |
| `lotserver_hd_thresh_us` | `120000` |
| `lotserver_hd_gain_boost` | `20` percent |
| `lotserver_probe_rtt_interval_ms` | `30000` |
| `lotserver_probe_rtt_duration_ms` | `150` |
| `lotserver_probe_rtt_cwnd_pct` | `50` percent |
| `lotserver_degraded_enable` | `1` |
| `lotserver_degraded_rate_min` | `6250000` bytes/s (50 Mbps) |
| `lotserver_degraded_rate_max` | `12500000` bytes/s (100 Mbps) |
| `lotserver_degraded_gain` | `20` (2.0x) |
| `lotserver_degraded_loss_pct` | `30` percent |
| `lotserver_degraded_recover_pct` | `20` percent |

The installer also persists:

```text
net.ipv4.tcp_congestion_control=lotspeed
net.ipv4.tcp_no_metrics_save=1
net.core.default_qdisc=fq
```

Preset and individual `lotspeed set` changes are saved in
`/etc/modprobe.d/lotspeed.conf`, so they survive a module reload or reboot.

The normal adaptive floor is 50% of the 250 Mbps ceiling, so ordinary
congestion stays at or above a 125 Mbps sender target. Degraded mode is
separate: RTT inflation and jitter never enter it by themselves. A flow must
reach 30% loss EWMA and remains degraded until loss EWMA falls to 20%. The
50-100 Mbps values are sender-side targets, not guaranteed goodput.

## Long-lived TCP Mux profile

For Nyanpass-style tunnel sockets, especially when the server also has many
user TCP connections:

```bash
lotspeed preset mux-throughput
```

This profile holds the target at 250 Mbps outside confirmed congestion and
uses 100% pacing. Adaptive control runs only in AVOIDING. Normal congestion
classification can still react to RTT inflation, but the 50-100 Mbps range is
reserved for a separate 30% severe-loss EWMA trigger and exits at 20%.
AVOIDING lasts at least 1 second. ECN or a TCP Loss state still enters
AVOIDING immediately without automatically enabling degraded mode.
The profile also reserves at least 250ms of target-rate flight data (about 8MB
or 5479 packets at MSS 1460). TCP send and receive buffers now start from a
512KB default and autotune up to 16MB. A 256KB `tcp_notsent_lowat` applies
application backpressure while retaining write batching, and a 1MB
`tcp_limit_output_bytes` cap bounds each socket's local qdisc/device backlog.
These thresholds do not cap sent-but-unacked flight data, retransmissions, or
total socket memory.

## What changed

1. Update delivery rate and loss once per packet-timed RTT instead of per ACK.
2. Ignore app-limited samples when they would lower the bandwidth estimate.
3. Compensate CWND for ACK aggregation common on WiFi and mobile access.
4. Calculate BDP from minimum RTT plus bounded jitter, not queued RTT.
5. Keep healthy and moderately congested flows out of the degraded rate cap.
6. Keep ProbeRTT at or above one BDP of the speed floor.
7. Require 20% loss, or sustained RTT inflation plus 8% loss, before congestion.
8. Prefer the `fq` qdisc while retaining TCP internal pacing as a fallback.
9. Add an optional minimum flight-time window for long-lived Mux tunnels.
10. Add congestion-only adaptive control with a configurable fast exit hold.
11. Retain the corrected byte accounting and Linux 6.10+ compatibility.
12. Bound per-socket TCP buffers at 16MB, use a 256KB unsent-data threshold,
    and cap local TCP output backlog at 1MB per socket.
13. Cap only sustained severe-loss flows at 100 Mbps with a 50 Mbps target
    floor, 2.0x CWND gain, and no extra minimum-flight window.
14. Use independent 30% enter and 20% recover thresholds so RTT inflation and
    ordinary congestion cannot trigger degraded mode.

## Deliberately not merged

The branch does not import the NeoQ queue, global connection-history tables,
or experimental Go control plane. Those branches add substantially more state
and lifetime complexity without enough evidence that they improve this fixed
rate deployment.

## Validation

Before production deployment, compile against the exact target kernel headers
and compare with the current `main` build using the same route and traffic:

```bash
iperf3 -c SERVER -P 4 -t 60
ping -i 0.2 SERVER
ss -tin
dmesg | grep -i lotspeed
```

Track throughput, retransmissions, RTT under load, and any module warnings.

## China Telecom ordinary 163 return path

For overseas servers sending to China Telecom over ordinary non-CN2 routes:

```bash
lotspeed preset ct-163-return
```

This uses adaptive rate control with a 250 Mbps per-connection ceiling,
100% pacing, and the same severe-loss-only 50-100 Mbps range. High-delay gain
compensation is disabled because ordinary 163 congestion is usually made
worse by filling a larger queue.

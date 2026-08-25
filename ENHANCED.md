# LotSpeed 3.6.4 Enhanced

This branch is a speed-first performance update on top of `main`.

## Recommended profile

```bash
sudo bash install.sh
lotspeed preset domestic-mixed
lotspeed status
```

The recommended preset keeps the 256 Mbps ceiling while adapting each TCP
connection independently for mixed fixed-line, WiFi, mobile and campus users:

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `32000000` bytes/s |
| `lotserver_gain` | `30` |
| `lotserver_beta` | `871` |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `10000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_congestion_only` | `0` |
| `lotserver_pacing_gain` | `105` percent |
| `lotserver_min_rate_pct` | `60` percent |
| `lotserver_min_flight_ms` | `0` (disabled) |
| `lotserver_avoid_hold_ms` | `500` milliseconds |
| `lotserver_loss_congest_pct` | `20` percent |
| `lotserver_loss_recover_pct` | `8` percent |
| `lotserver_rtt_confirm_samples` | `12` |
| `lotserver_rtt_tolerance_pct` | `60` percent |
| `lotserver_loss_guard` | `1` |
| `lotserver_noncong_beta` | `1000` |
| `lotserver_hd_enable` | `0` |
| `lotserver_hd_thresh_us` | `120000` |
| `lotserver_hd_gain_boost` | `20` percent |
| `lotserver_probe_rtt_interval_ms` | `60000` |
| `lotserver_probe_rtt_duration_ms` | `100` |
| `lotserver_probe_rtt_cwnd_pct` | `50` percent |

The installer also persists:

```text
net.ipv4.tcp_congestion_control=lotspeed
net.ipv4.tcp_no_metrics_save=1
net.core.default_qdisc=fq
```

Preset and individual `lotspeed set` changes are saved in
`/etc/modprobe.d/lotspeed.conf`, so they survive a module reload or reboot.

The 60% value is a sender-side target floor. It prevents the controller from
voluntarily backing off below 153.6 Mbps, but no TCP algorithm can guarantee
goodput above the physical bottleneck after loss and protocol overhead.

## Long-lived TCP Mux profile

For Nyanpass-style tunnel sockets, especially when the server also has many
user TCP connections:

```bash
lotspeed preset mux-throughput
```

This profile holds the target at 256 Mbps outside confirmed congestion and
keeps 105% pacing. Adaptive control runs only in AVOIDING, with a 75% (192 Mbps)
floor, a relaxed 30% pure-loss trigger, and an RTT-plus-25%-loss trigger that
must persist for 20 packet-timed rounds. The recovery threshold is 25%, RTT
confirmation decays rapidly after the path clears, and AVOIDING lasts at least
250ms. ECN or a TCP Loss state still enters AVOIDING immediately.
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
5. Keep the adaptive send target at or above 60% of the configured ceiling.
6. Keep ProbeRTT at or above one BDP of the speed floor.
7. Require 20% loss, or sustained RTT inflation plus 8% loss, before congestion.
8. Prefer the `fq` qdisc while retaining TCP internal pacing as a fallback.
9. Add an optional minimum flight-time window for long-lived Mux tunnels.
10. Add congestion-only adaptive control with a configurable fast exit hold.
11. Retain the corrected byte accounting and Linux 6.10+ compatibility.
12. Bound per-socket TCP buffers at 16MB, use a 256KB unsent-data threshold,
    and cap local TCP output backlog at 1MB per socket.

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

This uses adaptive rate control with a 256 Mbps per-connection ceiling,
95% pacing, 2.0x CWND gain, 75% congestion retention, and treats every loss
as a congestion signal. High-delay gain compensation is disabled because
ordinary 163 congestion is usually made worse by filling a larger queue.

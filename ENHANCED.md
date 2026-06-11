# LotSpeed 3.5.2 Enhanced

This branch is a conservative performance update on top of `main`.

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
| `lotserver_max_cwnd` | `6000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_pacing_gain` | `105` percent |
| `lotserver_min_rate_pct` | `60` percent |
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
```

Preset and individual `lotspeed set` changes are saved in
`/etc/modprobe.d/lotspeed.conf`, so they survive a module reload or reboot.

## What changed

1. Classify each TCP connection independently as stable, jittery or congested.
2. Measure adjacent-sample RTT jitter instead of treating all queue delay as jitter.
3. Require 20% loss, or sustained RTT inflation plus 8% loss, before congestion.
4. Keep adaptive throughput anchored to recent delivered rate.
5. Use a configurable adaptive floor; `domestic-mixed` keeps at least 60%.
6. Preserve ACK clock and in-flight data while congestion settles.
7. Use mode-specific CWND gain, pacing and loss retention.
8. Retain the corrected bandwidth sampling and Linux 6.10+ compatibility.

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

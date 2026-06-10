# LotSpeed 3.4 Enhanced

This branch is a conservative performance update on top of `main`.

## Recommended profile

```bash
sudo bash install.sh
lotspeed preset wan-enhanced
lotspeed status
```

The preset keeps the user's proven fixed-rate configuration:

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `32000000` bytes/s |
| `lotserver_gain` | `30` |
| `lotserver_beta` | `820` |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `6000` packets |
| `lotserver_adaptive` | `0` |
| `lotserver_pacing_gain` | `120` percent |
| `lotserver_loss_guard` | `1` |
| `lotserver_noncong_beta` | `972` |
| `lotserver_hd_enable` | `1` |
| `lotserver_hd_thresh_us` | `120000` |
| `lotserver_hd_gain_boost` | `20` percent |
| `lotserver_probe_rtt_interval_ms` | `30000` |
| `lotserver_probe_rtt_duration_ms` | `150` |
| `lotserver_probe_rtt_cwnd_pct` | `50` percent |

The installer also persists:

```text
net.ipv4.tcp_congestion_control=lotspeed
net.ipv4.tcp_no_metrics_save=1
```

Preset and individual `lotspeed set` changes are saved in
`/etc/modprobe.d/lotspeed.conf`, so they survive a module reload or reboot.

## What changed

1. Correct adaptive bandwidth samples from packets to bytes.
2. Use the four-argument congestion-control callback on Linux 6.10+.
3. Probe bandwidth on a time interval instead of increasing it on every ACK.
4. Refresh stale minimum RTT measurements when the route baseline changes.
5. Use RTT inflation to separate likely congestion from random WAN loss.
6. Retain part of the prior CWND during ProbeRTT.
7. Apply bounded high-delay CWND gain compensation.
8. Saturate rate and CWND arithmetic and validate module parameters.

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

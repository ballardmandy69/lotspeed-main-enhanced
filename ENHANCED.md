# LotSpeed 3.9.1 Enhanced

LotSpeed 3.9.1 keeps the upstream `main` fixed-rate behavior for healthy TCP Mux
connections and adds a cautious, per-connection efficiency guard for persistently
wasteful flows.

## Recommended profile

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v391.sh | sudo bash
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

Setting `lotserver_adaptive=0` disables the efficiency guard and restores the
fixed upstream-main target. No additional module parameters are introduced.

## Guard states

| State | Target with the main profile | Pacing gain |
| --- | ---: | ---: |
| `FULL` | 256 Mbps | 120% |
| `LIMIT_75` | 192 Mbps | 100% |
| `LIMIT_DYNAMIC` | frozen 100-192 Mbps | 100% |
| `PROBE_75` | 192 Mbps | 100% |
| `PROBE_FULL` | 256 Mbps | 120% |

A new transfer runs at full rate for a complete eight-second observation
window. It can move down only when that window transmits at least 70% of the
current target, is neither application-limited nor clearly receive-window
limited, contains at least 256 KiB and 16 retransmitted segments, and delivers
less than 50% of the transmitted TCP payload.

The first qualifying window moves only to `LIMIT_75`. A second qualifying
five-second window moves to `LIMIT_DYNAMIC`, whose ceiling is calculated once:

```text
clamp(five-second acknowledged delivery rate * 2.0, 100 Mbps, 192 Mbps)
```

That value is frozen until recovery. Compared with the 1.5 multiplier in 3.9,
the 2.0 multiplier preserves more sending headroom: an 80 Mbps acknowledged
rate freezes at 160 Mbps instead of 120 Mbps. It never follows a lower delivery
rate caused by its own limit, removing the recursive downshift behavior in
3.8.4.

## Recovery

Limited states reconsider recovery every five seconds. A qualifying recovery
window requires at least 80% efficiency, retransmitted bytes no greater than
20% of transmitted bytes, sustained activity, and no application or receive
window limitation.

Recovery probes one step at a time:

```text
LIMIT_DYNAMIC -> PROBE_75 -> LIMIT_75
LIMIT_75      -> PROBE_FULL -> FULL
```

Each probe lasts two seconds and must grow acknowledged delivery to at least
the previous target. A failed probe immediately restores the prior ceiling;
the next opportunity arrives after another five-second limited window. Thirty
seconds of real idle time clears the state and starts the next transfer at
`FULL`.

RTT, jitter, isolated losses, and client IP addresses do not directly select a
guard state. Each TCP connection is evaluated independently.

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

## Validation

```bash
iperf3 -c SERVER -P 1 -t 60
ss -tin
lotspeed status
sudo lotspeed logs 100
```

Guard transitions are logged only when `lotserver_verbose=1`.

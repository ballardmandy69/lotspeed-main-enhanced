# LotSpeed 3.10.9 Enhanced

LotSpeed 3.10.9 builds on the congestion-gated adaptive model from the 3.6.4
`mux-throughput` profile. Its floor follows 50% of the configured ceiling, it
restores broad Mux loss and RTT visibility, and clears stale per-connection
learning after sustained low Mux traffic. The main profile uses a 360 Mbps
ceiling, 2.6x CWND gain, and a 3% sustained moderate-loss threshold confirmed
over five qualified rounds. Both values are runtime-tunable.

## Install

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v3109.sh | bash
lotspeed preset mux-throughput
lotspeed status
```

## Main profile

| Parameter | Value |
| --- | ---: |
| `lotserver_rate` | `45000000` bytes/s (360 Mbps) |
| `lotserver_min_rate_pct` | `50` percent of ceiling |
| `lotserver_gain` | `26` (2.6x) |
| `lotserver_beta` | `871` (about 85% retained) |
| `lotserver_min_cwnd` | `32` packets |
| `lotserver_max_cwnd` | `10000` packets |
| `lotserver_adaptive` | `1` |
| `lotserver_pacing_gain` | `120` percent |
| `lotserver_min_flight_ms` | `250` ms |
| `lotserver_rtt_tolerance_pct` | `80` percent |
| `lotserver_loss_congest_pct` | `30` percent |
| `lotserver_loss_recover_pct` | `25` percent |
| `lotserver_loss_adapt_pct` | `3` percent |
| `lotserver_loss_adapt_samples` | `5` qualified rounds |
| `lotserver_rtt_confirm_samples` | `20` rounds |
| `lotserver_loss_guard` | `1` |
| `lotserver_noncong_beta` | `1000` |
| `lotserver_hd_enable` | `0` |

## Dynamic rate

Outside congestion avoidance, the target remains 360 Mbps. Stable paths use
120% pacing, jittery paths use at most 110% pacing, and confirmed congested
paths use 100% pacing. During confirmed congestion the target becomes:

```text
clamp(smoothed ACK arrival rate * 1.05, rate * 50%, rate)
```

The arrival estimate absorbs 25% of a higher sample and 12.5% of a lower
sample. Loss and RTT classification have separate eligibility gates. A loss
sample needs at least one delivered packet in a packet-timed RTT no longer
than two seconds. RTT evidence needs at least eight delivered packets over the
same maximum interval. Both accept `app_limited` Mux rounds.
Adaptation requires a 30% loss EWMA, 20 qualified rounds above the RTT
threshold with at least 25% loss EWMA, or sustained moderate loss. The
moderate path requires a round-aligned loss EWMA of at least 3% for five
accumulated qualified rounds. The evidence is held while EWMA is between about
2.2% and 3%, then decays by two per qualified round below about 2.2%. The RTT
threshold is the measured base RTT plus 80% and a jitter allowance. A single
RTO still reduces cwnd but cannot lower the target rate by itself. Once
congestion clears for 250 ms, the target returns to the configured ceiling.

Unlike 3.10.3, the loss numerator is no longer the loss newly marked by only
the current ACK. Both delivered and cumulative lost deltas now span the same
packet-timed RTT, preventing sustained loss from being diluted by a mismatched
delivery interval.

Unlike 3.10.5, a temporary empty send queue no longer blocks RTT learning, and
the minimum packet counts are close to the broad sampling behavior of 3.6.4.

After startup, an active non-app-limited round with a retransmission and an
ACK delivery rate below 70% of the configured ceiling also adds one moderate
adaptation round. The Mux activity gate excludes traffic below the existing
low-traffic reset threshold.

`lotserver_loss_adapt_pct` and `lotserver_loss_adapt_samples` can be changed
while the module is running and are persisted by `lotspeed set`. Lower values
admit persistent loss sooner; higher values are stricter. The classifier
remains quality-based and does not force a fixed percentage of connections
into adaptive mode.

## Low-traffic reset

Each TCP connection has a five-second activity window. Two consecutive windows
below 10% of the configured ceiling clear the ACK-rate estimate, loss EWMA,
moderate-loss and RTT-congestion evidence, avoidance state, and adaptive
target. With the main profile the threshold is 36 Mbps and the reset takes
10 seconds. A truly idle connection is also reset when transmission restarts
after 10 seconds.

The reset is per TCP connection and does not affect other clients or use an IP
address. It prevents a reused Mux from carrying a stale low-rate result into a
later transfer.

## Visibility

```bash
lotspeed rate-status
```

This on-demand command separates sockets that transmitted during the last ten
seconds into `ACTIVE_FULL_120`, `ACTIVE_JITTERY_110`, and
`ACTIVE_ADAPTIVE_100`.
Idle or stalled sockets, including stale adaptive pacing values, are shown
separately. `guard-status` remains as a compatibility alias.

## Mux buffers

```text
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.ipv4.tcp_rmem=8192 524288 16777216
net.ipv4.tcp_wmem=8192 524288 16777216
net.ipv4.tcp_notsent_lowat=262144
net.ipv4.tcp_limit_output_bytes=1048576
```

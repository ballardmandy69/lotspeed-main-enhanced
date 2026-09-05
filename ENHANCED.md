# LotSpeed 3.10.10 Enhanced

This release fixes loss-evidence lifecycle and idle reuse handling on top of
3.10.9. Control remains per underlying TCP, not per IP, AnyTLS substream, or a
presumed one-TCP-per-user relationship.

## Install

Run as root:

```bash
wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v31010.sh | bash
lotspeed status
lotspeed rate-status
```

Upgrades of a loaded module preserve supported runtime parameters and persist
them in the module configuration. Do not apply a preset unless you intend to
replace customized values. Offline configuration migration is not performed
when the old module is not loaded. Busy modules are not forcibly unloaded;
the previous default congestion algorithm is restored on unload failure.
For a fresh installation, lotspeed preset mux-throughput applies the default
module profile and buffer sysctls documented in README.md.

## Changes

- A separate paired delivered/lost snapshot is consumed only when eligible.
  Zero elapsed ticks or no delivery retains evidence for the next sample.
- Windows expire after two seconds without a qualifying sample. Expired loss
  EWMA and counts are cleared; an invalid window is not a synthetic healthy
  sample and does not itself clear the path classification.
- Moderate confirmation requires threshold-crossing EWMA and newly marked
  loss. One burst cannot keep increasing the count through old EWMA alone.
- The supplemental entry based on ACK speed below 70% of target is removed.
  No delivered/sent byte ratio replaces it.
- Pending unsent or unacknowledged data prevents idle reset, regardless of
  speed. A drained queue must be observed idle for about ten seconds before
  the next callback/restart clears history. Short TX_START events preserve
  loss snapshots. Recovery with no estimated flight is not treated as idle.

The indicator is delta_lost / (delta_delivered + delta_lost), not actual
retransmitted bytes, physical packet-loss probability, or remote application
goodput. Loss needs at least one delivered packet and a nonzero interval of
at most two seconds; RTT still needs eight packets over a nonzero interval
of at most two seconds. Both accept app-limited traffic. The bandwidth
estimator still rejects lower app-limited samples once it has an estimate.

The moderate default remains 3% with five fresh-loss confirmations. At about
2.2% or less, each qualified window removes two confirmations. Between
thresholds, or above threshold without fresh loss, evidence is held until
decay/expiry. Counts are accumulated, not strictly consecutive, and are not
seconds. A samples setting of one intentionally permits one qualifying burst
to trigger. The separate severe EWMA/RTT entry paths remain unchanged.

## Unchanged Defaults

| Parameter | Value |
| --- | ---: |
| rate | 45,000,000 bytes/s (360 Mbps) |
| minimum target | 50% of rate |
| CWND gain | 26 (2.6x) |
| beta / noncong_beta | 871 / 1000 |
| min / max CWND | 32 / 10000 packets |
| adaptive / high-delay compensation / verbose | on / off / off |
| moderate loss threshold / confirmations | 3% / 5 |
| severe loss threshold / recovery | 30% / 25% |
| RTT tolerance / confirmations | 80% plus jitter allowance / 20 |
| minimum flight / avoidance hold | 250ms / 250ms |

In AVOIDING, target remains clamp(smoothed ACK rate * 1.05, rate * floor%, rate).
Higher samples receive 25% weight; lower eligible samples receive 12.5%.
After classification recovery, avoidance exits once its minimum residence
time has elapsed, not after 250ms of continuous health. Pacing remains 120%
for stable, at most 110% for jittery, and at most 100% for congested paths.
Default targets are 180-360 Mbps with 432 Mbps stable pacing, not guaranteed
goodput. CWND, RTT, buffer and recovery thresholds are not retuned.

## Idle Reuse and Cost

Idle is callback-observed. A new transmission ends the idle period; repeated
small bursts less than ten seconds apart need not fully reset history.
An idle socket need not update its displayed pacing until another event
arrives. Pending data, zero-window stalls and RTOs do not imply healthy idle,
nor do they alone prove congestion. No per-socket timer, dynamic allocation,
logging loop or IP table is added. Private state stays within 88 bytes.
Rate-status remains a pacing inference, not internal-mode telemetry or a
count of saturated downloads.

## Verification

python3 tests/run_model_tests.py compiles production controller functions
against a TCP shim, exercising HZ=100/250/1000, pending evidence, isolated and
sustained losses, app-limited delivery, idle/backlog, counter wrap, expiry
and confirmation settings 1-255. CI also checks scripts, metadata and kernel
builds. These are not live network benchmarks or a guarantee that more flows
will enter adaptation.

Kernel sampling semantics:
[Linux 6.12 tcp_rate.c](https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_rate.c).

# LotSpeed Dup 3.10.17 Experimental

An optional, independently loadable TCP redundancy experiment. This is NOT the
3.10.16 management-only SACK/RACK/TLP helper, and NOT a replacement congestion
control. The 3.10.15 `lotspeed.c` algorithm and module remain unchanged.

Language/stack: C, Linux Netfilter and the normal IPv4/IPv6 output paths; Bash
for explicit runtime control. Builds against the running kernel headers, like
the existing module. Source compatibility targets Linux 4.9+, with actual
tested versions reported separately. Compilation is not a production safety
or performance certification. No livepatch, kprobe, private symbol lookup,
custom kernel, automatic startup service, or machine reboot is used.

## Trial Scope

- One explicitly configured **local TCP port**, or explicit **all local TCP ports**, in the initial network namespace
  (`init_net`) only. It does not operate on L3 forwarded traffic or container
  network namespaces. Loading it from a container does not change that scope.
- Enrolls connections only on observed outgoing SYN/SYN-ACK while enabled.
  Existing established connections are not retroactively enrolled. SYN data
  (TCP Fast Open) is not enrolled. Retransmitted SYNs do not reset the allowance.
- First 128 KiB of original TCP sequence space is eligible for one optional
  copy per range. This is an eligibility window, NOT a promise to duplicate
  all 128 KiB: ACK cancellation, resource limits and optional budgets take precedence.
- After the prefix, the latest original segment is a tail candidate. New data
  cancels the previous tail candidate. Packet-level silence is a heuristic:
  it is NOT a business-message boundary or proof that the TCP send queue is
  empty. An application-limited or cwnd-limited pause can look like a tail.
- Default delay is at least about 5 ms from observation (or a plausible future
  monotonic EDT). Scheduling uses kernel jiffies/workqueues: on HZ=100 it cannot
  provide 5 ms precision. It does not know the actual NIC transmission time.
- Copies use fresh linear skbs with complete software checksums. GSO payloads
  are selected by byte range, not cloned wholesale. Originals are not modified
  or software-segmented by this module. At most 64 prefix segments are examined
  per observed original skb. Oversized/unsupported candidates are skipped.
- Plain IPv4 and IPv6 TCP only. IP options, fragments, IPv6 extension headers,
  TCP MD5/AO/MPTCP, urgent data and unsupported GSO types are skipped. FIN/RST
  ends protection (including half-closed connections), not the TCP connection.

## Limits and Semantics

`load all` selects all local ports with `budgeted=0`: no percentage/rate filtering.
Every supported new flow can contribute prefix/tail candidates, including SSH
and other management traffic. This is NOT a full-stream mirror, a loss detector,
or a guarantee that every connection gets a copy. A missing ACK after 5 ms is
not proof of loss, especially when the RTT is much longer.

Single-port loads retain `budgeted=1`, `budget_pct=10`, `max_mbps=10` by default.
Both modes default to `delay_ms=5`. In unbudgeted mode `budget_pct` and
`max_mbps` remain visible but are ignored. The following credit/rate rules apply
only to budgeted mode:
The ratio earns copy credit only from observed **new data** IP bytes of tracked
flows, not normal retransmissions or unrelated host traffic. Credit starts at
zero and is capped at 64 KiB. It is reserved before candidate allocation and
charged at submission. ACK cancellation releases reservations, not new credit.
The independent decimal-Mbps token bucket allows a bounded burst of 10 ms of
its rate (at least 2048 bytes). Both controls must permit a copy; otherwise it
is skipped, never postponed into an unbounded backlog. Neither counts Ethernet
or encapsulation overhead. These are module-wide limits, not per-user fairness.

Resource defaults: 131072 tracked flows, 32768 hash buckets, 131072 queued
candidates, 128 candidates per flow, and 64 MiB of charged queued skb/item memory.
`max_flows` accepts 1..262144; `max_queue_mb` accepts 1..512 MiB at load time.
Flow metadata and a bounded
in-progress allocation/submission are additional. A flow idle for 120 seconds
is discarded and will not be re-enrolled without a new SYN. Healthy original
TCP continues if tables are full, allocation fails or the copy budget runs out.
There is CPU cost from hooks, tracking and copying, even when most candidates
are cancelled; no claim of zero throughput/latency impact is made. Expiry scans
are incremental and disable purges in small batches, but packet paths still
share a global lock. The table capacity is NOT a certification of throughput
with 100000 simultaneously active production connections. Queue capacity can
be exhausted well before all flows have pending copies. Candidates more than
100 ms past their deadline are skipped rather than sent as a stale burst.

ACK/SACK/FIN/RST observations and overlapping outgoing retransmissions cancel
pending candidates conservatively. Partial coverage cancels the entire candidate.
Incoming ACKs are observed before TCP validates them; this is **not** the TCP
SACK scoreboard. Cancellation is best-effort up to the worker's submission
boundary. A later normal retransmission/TLP may still overlap a submitted copy.
Copies already handed to IP/qdisc/driver cannot be recalled, even by disable.

Copies have no originating socket, so they cannot recursively generate more
copies. They re-enter the ordinary IP output/filter path before conntrack/NAT;
they do not bypass firewall rules. Socket-owner/cgroup-based policy may treat
them differently, and route/policy changes during the delay are not reconciled
with the owning TCP socket. Do not first deploy this on a policy-routed,
authenticated or encapsulated production path.

Budgeted copies have an **independent** extra-traffic budget; unbudgeted mode
has no bandwidth cap. Neither mode debits the
original socket's native pacing timeline and do not count in its `bytes_retrans`.
They can produce DSACKs and affect TCP feedback indirectly. Compare module
submission counters with packet captures, receiver goodput, latency and CPU;
do not infer benefit from `ss` retransmission ratios alone. Random losses may
benefit; persistent congestion and correlated losses can get worse. This does
not fix proxy connection lifecycle problems or guarantee CLOSE_WAIT recovery.

## Build and Run

### Single-file Installer

`install-lotspeed-dup-3.10.17.sh` embeds these sources with an archive SHA-256
integrity check. Download to a file, then run as root with Bash; do not pipe it
to Bash. Build tools, kmod and headers for the running kernel must already be
installed. It never runs apt/yum, changes CC/qdiscs/sysctls, or installs a boot service.

```bash
bash install-lotspeed-dup-3.10.17.sh
bash install-lotspeed-dup-3.10.17.sh status
bash install-lotspeed-dup-3.10.17.sh disable
bash install-lotspeed-dup-3.10.17.sh unload
```

Default install builds and checks module metadata before unloading the old
companion, then performs `load all` and `enable`. This resets companion tracking;
established connections are not enrolled again. A build failure leaves the
loaded module untouched. A later unload/load/enable failure stops the script
and does not automatically restore the previous module or its tracking state.
Builds are retained under `/root/lotspeed-dup-builds/` for diagnostics; the old
`/root/companion` directory is not overwritten. Control actions do not compile.
An embedded checksum detects payload damage, not a malicious replacement of
both script and checksum. Obtain the file from the published repository/release.

### Manual Build

First use a disposable test VM with matching headers, a compiler and make.
Do not install the old module installer or change presets for this experiment.
The commands below assume root only for load/enable/disable/unload.

```bash
cd companion
bash dupctl build
# 5201 is an EXAMPLE test port, not a production default.
bash dupctl load 5201
bash dupctl status
bash dupctl enable
# Create new test TCP connections now; existing ones are intentionally ignored.
bash dupctl status
```

Optional load-time arguments:

```bash
bash dupctl load 5201 10 10 5
#                port Mbps percent delay_ms
bash dupctl load 5201 10 10 5 131072
#                                max_flows
bash dupctl load all
# All ports, no percentage/rate budget, 131072 flows, 64 MiB queue, 5 ms.
bash dupctl load all 131072 64 5
#                   flows MiB delay_ms
```

Loading is always disabled; `insmod ... enabled=1` is rejected. Limits/scope
are read-only after loading. Change them by explicitly disabling/unloading and
loading again, without disturbing the congestion-control module or existing
TCP connections. Secure Boot/module-signing policies still apply: this tool
does not bypass them. Missing headers or unsupported build APIs are errors,
not a reason to guess a different kernel's binary or force-load it.

Stop and remove:

```bash
bash dupctl disable
bash dupctl unload
```

Disable waits for the sender worker and purges flow/candidate state. No further
copies are submitted after disable returns, but previously submitted packets
may still leave downstream queues. No sysctl, CC, qdisc, persistent config,
or application socket is changed by these control commands.

## Counters

`bash dupctl status` reads `/sys/module/lotspeed_dup/parameters/stats`:

- `original_ip_bytes`: observed new data including estimated IP/TCP headers.
- `submitted_copies` / `submitted_ip_bytes`: handed to IP, not proven delivered.
- `cancelled`: discarded queued candidates, including budget/age/disable cases.
- `skipped_budget`: insufficient reserved credit/rate budget (budgeted mode only).
- `skipped_late`: candidate more than 100 ms past its deadline.
- `skipped_limit`: capacity, allocation, or packet construction/MTU skips.
- `skipped_packet`: parse/format exclusions, including unrelated protocols.
- `output_errors`: immediate IP output errors; later qdisc/NIC losses may be invisible.
- `flows`, `pending`, `queue_bytes`: current tracked/queued resources.

Counters survive disable/enable but reset on unload. Credit resets to zero on
each enable. There are no per-packet logs, polling daemons or TCP statistics resets.

## Verification

Policy, packet-construction and isolated-VM tests live in the parent `tests/`
directory. The VM harness must not load this module on the development host or
a production server. Keep an unmodified 3.10.15/BBR baseline, first validate
feature-off behavior, then compare feature-on with identical test traffic.
Do not publish performance claims based only on model or compilation tests.

### 3.10.17 Checks (2026-11-17)

- `W=1` builds and modpost passed for 5.15.0-203-generic, 6.1.0-50-amd64,
  and 6.8.0-138-generic, x86_64. Missing vmlinux only skipped BTF generation.
- Repository unittest discovery passed 65 tests, including scope boundaries,
  mock-only CLI lifecycle/legacy compatibility, policy and packet checks.
- The unchanged congestion controller passed its HZ=100/250/1000 regressions.
- Isolated Debian 6.1 QEMU passed single-port and unbudgeted all-port cases:
  active/passive opens on 5201/5202, GSO originals, checked copy payloads and
  checksums, ACK cancellation, synchronous disable and unload. Single-port mode
  excluded 5202; all-port mode copied candidates on both ports without budget skips.
- A separate guest-only SYN-cookie test tracked 100000 distinct SYN/SYN-ACK
  tuples and disabled/purged them successfully (2556 ms creation, 55 ms disable
  in this one TCG run). This verifies table capacity/cleanup, NOT 100000
  established connections, data-copy throughput, full cookie handshakes,
  idle-expiry stress, or production latency. Do not treat these timings as a benchmark.

### Baseline Verification and Remaining Limits

- The previous 3.10.16 baseline passed real `W=1` builds and modpost: 5.15.0-203-generic,
  6.1.0-50-amd64, 6.8.0-138-generic, x86_64. BTF was skipped without vmlinux.
- Linux 4.9 is a source compatibility target only: archived build dependencies
  could not be obtained, so 4.9 is **not compile-verified**. Neither are 6.10+
  and vendor-specific kernel builds. No foreign `.ko` should be force-loaded.
- Actual parser/copy functions were extracted for GCC/Clang UBSan tests with
  independent IPv4/IPv6 wire-checksum and payload checks. Sequence wrap,
  prefix boundaries, malformed formats and allocation failures were covered.
- Isolated QEMU with Debian 6.1.0-50: IPv4 active and passive-open transfers,
  prefix/tail candidates, GSO originals, software-checksummed copies, fast ACK
  cancellation, synchronous disable with original flow continuing, and unload.
  Production defaults (10 percent / 10 Mbps) passed twice; one active transfer
  submitted 15309 IP bytes against a 27098-byte percentage allowance, with
  budget skips observed. A separate 100 percent / 1000 Mbps diagnostic mode
  exercised more prefix/tail copies. These are functional tests, not benchmarks.
- The synthetic peer uses a 25 ms handshake RTT and delayed data ACKs. A near-zero
  handshake RTT with delayed data ACKs can trigger native TCP probes/retransmits;
  those are not companion copies. Diagnostic mode preserves the strict check,
  rather than silently treating native retransmissions as successful duplicates.
- Not runtime-validated: IPv6, SYN cookies, SACK blocks, correlated/random WAN
  loss, actual absolute-rate/burst measurements, high connection counts, NIC
  hardware offloads, BBR-specific performance, and production proxy workloads.
  Percentage accounting was checked in the VM; absolute bucket math was unit
  tested, not certified with a physical wire-rate measurement.

The VM harness refuses execution without its guest-only command-line marker;
it has no external NIC or host module loading. Test packages intentionally ship
source, not a binary built for a different machine's kernel.

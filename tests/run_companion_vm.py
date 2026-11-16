#!/usr/bin/env python3
"""Build and boot an isolated x86_64 Linux smoke-test VM (Linux/WSL host).

Requires a matching kernel/module, static busybox, gcc with static libc, and QEMU.
No host root, insmod, TUN, network configuration, mounted disk, or NIC is used.
Example (paths relative to repository):
  python3 tests/run_companion_vm.py --kernel outputs/kernel/bzImage \
      --module outputs/build/lotspeed_dup.ko --busybox /usr/bin/busybox \
      --extra-module outputs/kernel/tun.ko
Repeat --extra-module in dependency load order if the kernel needs more modules.
Defaults exercise budget_pct=10/max_mbps=10. Use --budget-pct 100 --max-mbps 1000
for mandatory prefix/tail/GSO coverage. Totals do not prove a sliding-window rate/burst bound.
Single-port mode includes 5201 and excludes 5202 with budgeted=1.
Explicit --all-ports sets port=0/all_ports=1/budgeted=0 and must capture both ports,
including prefix/tail copies exceeding the former default 10% cap. It does not copy every byte.
Both ports exercise active and passive TCP opens, not just module parameter readback.
This sequential functional test does NOT validate 100,000 concurrent TCP connections.
Optional --all-ports --synthetic-flows 100000 verifies SYN-only cookie flow tracking
and disable/purge inside the guest, not 100,000 established connections or production performance.
All generated artifacts and compiler temporary files stay in outputs/.
@author lyt
@date 2026-11-17
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def static_elf(path):
    """Reject non-x86_64 or dynamically linked guest executables without executing them."""
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 62:
        raise ValueError(f"Expected little-endian x86_64 ELF: {path}")
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    if entry_size < 56 or offset + entry_size * count > len(data):
        raise ValueError(f"Malformed ELF program headers: {path}")
    for index in range(count):
        if struct.unpack_from("<I", data, offset + entry_size * index)[0] == 3:
            raise ValueError(f"Guest executable has PT_INTERP, use static build: {path}")
    return data


def initramfs(entries, destination):
    """Write Linux newc directly, including executable modes; no host root/cpio needed."""
    with destination.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as archive:
        for inode, (name, mode, data) in enumerate(entries + [("TRAILER!!!", 0, b"")], 1):
            encoded = name.encode("ascii") + b"\0"
            fields = (inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0,
                      len(data), 0, 0, 0, 0, len(encoded), 0)
            header = b"070701" + b"".join(f"{field:08x}".encode("ascii") for field in fields)
            archive.write(header + encoded)
            archive.write(b"\0" * (-(len(header) + len(encoded)) % 4))
            archive.write(data)
            archive.write(b"\0" * (-len(data) % 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--busybox", type=Path, required=True)
    parser.add_argument("--extra-module", type=Path, action="append", default=[])
    parser.add_argument("--cc", default="gcc", help="Linux x86_64 compiler executable")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=int, default=180, help="QEMU wall-clock limit, seconds")
    parser.add_argument("--handshake-ms", type=int, default=25,
                        help="Seed RTT before 25ms data ACKs; 0 diagnoses spurious TCP tail probes")
    parser.add_argument("--budget-pct", type=int, default=10, help="Guest module copy budget, 1..100 percent")
    parser.add_argument("--max-mbps", type=int, default=10, help="Guest module copy rate cap, 1..1000 Mbps")
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--all-ports", action="store_true", help="Guest port=0/all_ports=1/budgeted=0")
    scope.add_argument("--single", dest="all_ports", action="store_false", help="Guest port=5201/budgeted=1")
    parser.set_defaults(all_ports=False)
    parser.add_argument("--synthetic-flows", type=int, default=0,
                        help="Optional guest SYN-only state count, 0..100000; requires --all-ports")
    parser.add_argument("--output", type=Path, default=ROOT / "outputs/companion-vm")
    parser.add_argument("--build-only", action="store_true", help="Compile/package; does NOT claim runtime success")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run in Linux/WSL; host root is not needed")
    if not 10 <= args.timeout <= 600:
        parser.error("--timeout must be between 10 and 600 seconds")
    if not 0 <= args.handshake_ms <= 50:
        parser.error("--handshake-ms must be between 0 and 50")
    if not 1 <= args.budget_pct <= 100 or not 1 <= args.max_mbps <= 1000:
        parser.error("--budget-pct must be 1..100 and --max-mbps must be 1..1000")
    if not 0 <= args.synthetic_flows <= 100000 or (args.synthetic_flows and not args.all_ports):
        parser.error("--synthetic-flows must be 0..100000 and nonzero requires --all-ports")
    outputs = (ROOT / "outputs").resolve()
    destination = args.output.resolve()
    if destination != outputs and outputs not in destination.parents:
        parser.error("--output must stay inside this repository's outputs/")
    for source in [args.kernel, args.module, args.busybox, *args.extra_module]:
        if not source.is_file():
            parser.error(f"Missing input: {source}")
    compiler = shutil.which(args.cc)
    qemu = shutil.which(args.qemu)
    if not compiler or (not args.build_only and not qemu):
        parser.error("Compiler/QEMU executable not found")
    destination.mkdir(parents=True, exist_ok=True)
    # Unique run paths prevent another concurrent test or a stale log from faking PASS.
    run = Path(tempfile.mkdtemp(prefix="run-", dir=destination))
    temporary = run / "tmp"
    temporary.mkdir()
    env = dict(os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
    print(f"ARTIFACTS: {run}", flush=True)
    binary = run / "companion_vm"
    command = [compiler, "-static", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
               str(ROOT / "tests/companion_vm.c"), "-o", str(binary)]
    compile_result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=90)
    (run / "compile.log").write_text(compile_result.stdout + compile_result.stderr)
    if compile_result.returncode:
        raise RuntimeError(f"C compile failed: {compile_result.stderr}")
    entries = [(name, stat.S_IFDIR | 0o755, b"")
               for name in ("bin", "dev", "proc", "sys", "tmp", "modules")]
    entries += [("bin/busybox", stat.S_IFREG | 0o755, static_elf(args.busybox)),
                ("bin/sh", stat.S_IFLNK | 0o777, b"busybox"),
                ("companion_vm", stat.S_IFREG | 0o755, static_elf(binary)),
                ("init", stat.S_IFREG | 0o755,
                 (ROOT / "tests/companion_vm_init.sh").read_bytes().replace(b"\r\n", b"\n")),
                ("modules/lotspeed_dup.ko", stat.S_IFREG | 0o644, args.module.read_bytes())]
    entries += [(f"modules/dep-{index:03d}.ko", stat.S_IFREG | 0o644, module.read_bytes())
                for index, module in enumerate(args.extra_module)]
    archive = run / "initramfs.cpio.gz"
    initramfs(entries, archive)
    kernel = run / "kernel"
    shutil.copyfile(args.kernel, kernel)
    manifest = {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in [args.kernel, args.module, args.busybox, *args.extra_module,
                             ROOT / "tests/companion_vm.c", ROOT / "tests/companion_vm_init.sh",
                             Path(__file__).resolve()]}
    (run / "inputs-sha256.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.build_only:
        print("BUILD ONLY: static -Wall -Wextra -Werror compile and initramfs complete; runtime NOT tested")
        return 0
    # Deliberately no NIC, disks, filesystem sharing, host TUN, or host module commands.
    command = [qemu, "-machine", "pc,accel=tcg", "-cpu", "max", "-smp", "1", "-m", "256",
               "-nodefaults", "-no-user-config", "-display", "none", "-monitor", "none",
               "-serial", "stdio", "-nic", "none", "-no-reboot", "-kernel", str(kernel),
               "-initrd", str(archive), "-append",
               "console=ttyS0 rdinit=/init panic=1 oops=panic lotspeed_dup_test=1 "
               f"lotspeed_dup_handshake_ms={args.handshake_ms} "
               f"lotspeed_dup_budget_pct={args.budget_pct} lotspeed_dup_max_mbps={args.max_mbps} "
               f"lotspeed_dup_all_ports={int(args.all_ports)} "
               f"lotspeed_dup_synthetic_flows={args.synthetic_flows}"]
    (run / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    log_path = run / "serial.log"
    timed_out = False
    with log_path.open("wb") as log:
        with subprocess.Popen(command, cwd=run, env=env, stdin=subprocess.DEVNULL,
                              stdout=log, stderr=subprocess.STDOUT) as process:
            try:
                status = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                process.kill()
                status = process.wait(timeout=10)
    serial = log_path.read_text(errors="replace")
    print(serial, end="", flush=True)
    lines = [line.strip() for line in serial.splitlines()]
    passed = (not timed_out and status == 0 and lines.count("COMPANION_VM_RESULT=0") == 1
              and lines.count("COMPANION_VM_C_PASS") == 1 and "COMPANION_VM_RESULT=1" not in lines
              and not any(marker in serial for marker in
                          ("COMPANION_VM_FAIL", "Kernel panic", "BUG:", "WARNING:", "Oops:")))
    if args.synthetic_flows:
        passed = passed and any(line.startswith(f"SYNTHETIC_STATE_PASS target={args.synthetic_flows} ")
                                for line in lines)
    result = {"passed": passed, "qemu_exit": status, "timed_out": timed_out,
              "serial_log": str(log_path),
              "budget_pct": args.budget_pct, "max_mbps": args.max_mbps,
              "handshake_ms": args.handshake_ms,
              "all_ports": args.all_ports, "scope_port": 0 if args.all_ports else 5201,
              "budgeted": not args.all_ports,
              "test_kind": "functional+synthetic-state" if args.synthetic_flows else "sequential-functional",
              "synthetic_flow_target": args.synthetic_flows, "validates_100k_connections": False,
              "coverage": "IPv4 active/passive-open 5201/5202 scope; no TCP ACK-validation claim"}
    (run / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"VM {'PASS' if passed else 'FAIL'}: {log_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"VM FAIL: {error}", file=sys.stderr)
        raise SystemExit(1) from error

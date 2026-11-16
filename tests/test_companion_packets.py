#!/usr/bin/env python3
"""Test extracted production C against Linux UAPI layouts and independent wire checks.

Requires Linux/WSL, a C compiler with UBSan and installed Linux UAPI headers.
Only skb/dst allocation, copy and checksum primitives are mocked. No hooks,
timers, socket stack, routing, XFRM, real offloads or module loading are tested.
The copy helper's caller must provide nonzero MSS and an in-payload range.
"""
import json
import os
from pathlib import Path
import re
import resource
import shlex
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
PRODUCTION = ROOT / "companion/lotspeed_dup.c"


def extract(source, kind, name):
    if kind == "struct":
        pattern = r"^struct " + re.escape(name) + r" \{.*?^\};"
    else:
        pattern = r"^static [^;{]*\b" + re.escape(name) + r"\([^;{]*\)\s*\{.*?^\}"
    matches = list(re.finditer(pattern, source, re.M | re.S))
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one production {kind} {name}; found {len(matches)}")
    match = matches[0]
    line = source.count("\n", 0, match.start()) + 1
    return f'#line {line} "{PRODUCTION.as_posix()}"\n' + match.group(0)


def checksum(data):
    """Independent network-byte-order one's-complement oracle."""
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def disable_child_core_dump():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class CompanionPacketTests(unittest.TestCase):
    """Validate real parser/copy logic, not a Python policy translation.

    @author lyt
    @date 2026-11-17
    """

    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="companion_packet_", dir=ROOT / "tests")
        cls.addClassCleanup(cls.directory.cleanup)
        folder = Path(cls.directory.name)
        source = PRODUCTION.read_text(encoding="utf-8")
        unit = '\n'.join([
            '#include "tests/companion_packet_shim.h"',
            extract(source, "struct", "dup_key"),
            extract(source, "struct", "dup_packet"),
            extract(source, "function", "dup_parse"),
            extract(source, "function", "dup_copy_range"),
            '#include "tests/companion_packet_cases.c"',
        ])
        harness = folder / "packets.c"
        harness.write_text(unit, encoding="utf-8")
        cls.binaries = []
        for optimization in ("-O0", "-O2"):
            binary = folder / ("packets" + optimization)
            result = subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=gnu99",
                "-Wall", "-Wextra", "-Werror",
                *shlex.split(os.environ.get("CFLAGS", "")), optimization,
                "-UNDEBUG", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                "-I", str(ROOT), str(harness), "-o", str(binary),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            cls.binaries.append(binary)

    def run_case(self, binary, case):
        result = subprocess.run([str(binary), case], cwd=ROOT, capture_output=True,
                                text=True, timeout=30, preexec_fn=disable_child_core_dump)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout

    def test_packet_checksums_payload_and_preserved_headers(self):
        for binary in self.binaries:
            records = [json.loads(line) for line in self.run_case(binary, "construction").splitlines()]
            self.assertEqual(len(records), 46)
            for record in records:
                with self.subTest(binary=binary.name, family=record["family"], label=record["label"],
                                  offset=record["offset"]):
                    self.check_packet(record)

    def check_packet(self, record):
        original = bytes.fromhex(record["original"])
        copied = bytes.fromhex(record["copy"])
        family = record["family"]
        ip_length = 20 if family == 4 else 40
        offset, length = record["offset"], record["length"]
        tcp_length = (original[ip_length + 12] >> 4) * 4
        header = ip_length + tcp_length
        payload = len(original) - header
        self.assertEqual(copied[0] >> 4, family)
        self.assertEqual(len(copied), header + length)
        self.assertEqual(copied[header:], original[header + offset:header + offset + length])
        self.assertEqual(copied[ip_length + 20:header], original[ip_length + 20:header])
        self.assertEqual(struct.unpack_from("!I", copied, ip_length + 4)[0],
                         (0xfffffc00 + offset) & 0xffffffff)
        self.assertEqual(bool(copied[ip_length + 13] & 0x80), offset == 0)
        self.assertEqual(bool(copied[ip_length + 13] & 0x08), offset + length == payload)
        if record["label"].startswith("prefix-"):
            self.assertLessEqual(offset + length, 1501)
        if record["label"] == "prefix-partial":
            self.assertEqual((offset, length), (1460, 41))
        expected = bytearray(original[:header] + original[header + offset:header + offset + length])
        struct.pack_into("!I", expected, ip_length + 4, (0xfffffc00 + offset) & 0xffffffff)
        if offset:
            expected[ip_length + 13] &= ~0x80
        if offset + length < payload:
            expected[ip_length + 13] &= ~0x08
        expected[ip_length + 16:ip_length + 18] = b"\0\0"
        tcp_size = tcp_length + length
        if family == 4:
            struct.pack_into("!H", expected, 2, len(copied))
            struct.pack_into("!H", expected, 4, (65534 + offset // record["mss"]) & 0xffff)
            expected[10:12] = b"\0\0"
            struct.pack_into("!H", expected, 10, checksum(bytes(expected[:20])))
            self.assertEqual(checksum(copied[:20]), 0)
            pseudo = copied[12:20] + struct.pack("!BBH", 0, 6, tcp_size)
        else:
            struct.pack_into("!H", expected, 4, tcp_size)
            pseudo = copied[8:40] + struct.pack("!I3xB", tcp_size, 6)
        self.assertEqual(checksum(pseudo + copied[ip_length:]), 0)
        struct.pack_into("!H", expected, ip_length + 16, checksum(pseudo + bytes(expected[ip_length:])))
        self.assertEqual(copied, bytes(expected), "Unexpected mutation of a preserved IP/TCP field")

    def test_truncation_fragments_extensions_and_options(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name):
                self.run_case(binary, "parser")

    def test_allowed_and_forbidden_gso_types(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name):
                self.run_case(binary, "gso")

    def test_mtu_missing_route_allocation_and_copy_failures(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name):
                self.run_case(binary, "failures")

    def test_original_gso_wire_accounting_above_65535(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name):
                self.run_case(binary, "large-wire")

    def test_gso_requires_matching_tcp_base_type(self):
        # Flags alone must never substitute for the matching TCP GSO base type.
        outputs = [self.run_case(binary, "gso-missing-base") for binary in self.binaries]
        self.assertEqual(outputs, ["invalid_gso_accepted=0\n"] * len(self.binaries))


if __name__ == "__main__":
    unittest.main(verbosity=2)

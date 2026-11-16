#!/usr/bin/env python3
"""Compile the real, dependency-free policy and run it under mandatory UBSan."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CASES = ("before", "overlap", "prefix", "init", "credit", "take", "idle",
         "clock", "ratios", "rates")


class CompanionPolicyTests(unittest.TestCase):
    """Exercise policy without loading a module or changing system settings.

    @author lyt
    @date 2026-11-17
    """

    def test_freestanding_header_has_no_runtime_dependencies(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        source = """
typedef _Bool bool;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int s32;
#include "companion/policy.h"
bool check(struct lsdup_budget *b, u32 n, u32 rate, u64 t)
{
    lsdup_budget_init(b, t);
    lsdup_budget_earn(b, n, 50);
    return lsdup_budget_take(b, n, rate, t + 1000000U);
}
"""
        with tempfile.TemporaryDirectory(prefix=".companion-policy-", dir=ROOT / "tests") as temp:
            obj = Path(temp) / "policy.o"
            for optimization in ("-O0", "-O2"):
                with self.subTest(optimization=optimization):
                    result = subprocess.run([
                        *compiler, "-m32", optimization, "-std=gnu89", "-Wall", "-Wextra",
                        "-Werror", "-ffreestanding", "-fno-pic", "-fno-stack-protector",
                        "-I", str(ROOT), "-x", "c", "-c", "-", "-o", str(obj),
                    ], input=source, cwd=ROOT, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    symbols = subprocess.run(["nm", "-u", str(obj)], cwd=ROOT,
                                             capture_output=True, text=True, timeout=15)
                    self.assertEqual(symbols.returncode, 0, symbols.stderr)
                    self.assertEqual(symbols.stdout.strip(), "", symbols.stdout)

    def test_policy_with_ubsan(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix=".companion-policy-", dir=ROOT / "tests") as temp:
            binary = Path(temp) / ("policy.exe" if os.name == "nt" else "policy")
            for optimization in ("-O0", "-O2"):
                with self.subTest(optimization=optimization, stage="compile"):
                    command = [
                        *compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                        "-Wdeclaration-after-statement",
                        *shlex.split(os.environ.get("CFLAGS", "")), optimization,
                        "-UNDEBUG", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        "-I", str(ROOT), str(ROOT / "tests/companion_policy_cases.c"),
                        "-o", str(binary),
                    ]
                    result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                            text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if result.returncode:
                    continue
                for case in CASES:
                    with self.subTest(optimization=optimization, case=case):
                        result = subprocess.run([str(binary), case], cwd=ROOT,
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(result.stdout.strip(), case + ": ok")
                        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main(verbosity=2)

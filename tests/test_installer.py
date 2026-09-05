#!/usr/bin/env python3
"""Exercise install_module against an isolated fake filesystem and commands."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "install.sh").read_text(encoding="utf-8")
BODY = re.search(r"^install_module\(\) \{.*?^\}", SOURCE, re.M | re.S).group(0)
REQUIRED = re.search(r"local required_parameters=\((.*?)\)", BODY, re.S).group(1).split()


class InstallerTests(unittest.TestCase):
    def exercise(self, loaded=True, busy=False):
        with tempfile.TemporaryDirectory(prefix="lotspeed-installer-") as temp:
            root = Path(temp)
            config = root / "etc/modprobe.d/lotspeed.conf"
            params = root / "sys/module/lotspeed/parameters"
            available = root / "proc/sys/net/ipv4/tcp_available_congestion_control"
            build = root / "build"
            for path in (config.parent, params, available.parent, build,
                         root / "lib/modules/test/kernel/net/ipv4"):
                path.mkdir(parents=True, exist_ok=True)
            values = dict.fromkeys(REQUIRED, "1")
            values.update(lotserver_rate="40000000", lotserver_gain="26",
                          lotserver_min_rate_pct="50",
                          lotserver_loss_adapt_pct="3",
                          lotserver_loss_adapt_samples="2",
                          lotserver_adaptive="Y", lotserver_verbose="N")
            for name, value in values.items():
                (params / name).write_text(value)
            (params / "lotserver_removed").write_text("42")
            (params / "lotserver_bad").write_text("1 unexpected=2")
            (root / "supported").write_text("\n".join(
                name + ":test parameter" for name in [*values, "lotserver_bad"]))
            (params.parent / "version").write_text("3.10.10-enhanced")
            (build / "lotspeed.ko").write_text("fake compiled module")
            available.write_text("reno cubic lotspeed")
            config.write_text("options lotspeed lotserver_removed=42\n")
            body = re.sub(r"/(?:sys/module/|etc/modprobe\.d/|proc/sys/)",
                          lambda match: root.as_posix() + match.group(0), BODY)
            prelude = r'''
set -Eeuo pipefail
info() { printf '%s\n' "$*"; }
fail() { printf '%s\n' "$*" >&2; exit 1; }
choose_fallback_cc() { printf 'cubic\n'; }
lsmod() { if [[ "$TEST_LOADED" == 1 ]]; then printf 'lotspeed 36864 2\n'; fi; }
modinfo() { cat "$TEST_ROOT/supported"; }
sysctl() {
    if [[ "$1" == -n ]]; then
        printf 'lotspeed\n'
    else
        printf '%s\n' "$2" >> "$TEST_ROOT/sysctl.log"
    fi
}
rmmod() { [[ "$TEST_BUSY" == 0 ]]; }
depmod() { :; }
modprobe() { printf '%s\n' "$@" > "$TEST_ROOT/modprobe.log"; }
MODULE_NAME=lotspeed
VERSION=3.10.10-enhanced
INSTALL_DIR="$TEST_ROOT/build"
MODULE_DEST="$TEST_ROOT/lib/modules/test/kernel/net/ipv4/extra"
LEGACY_MODULE="$TEST_ROOT/lib/modules/test/kernel/net/ipv4/lotspeed.ko"
'''
            script = root / "exercise.sh"
            script.write_text(prelude + "\n" + body + "\ninstall_module\n",
                              encoding="utf-8", newline="\n")
            env = dict(os.environ, TEST_ROOT=root.as_posix(),
                       TEST_LOADED=str(int(loaded)), TEST_BUSY=str(int(busy)))
            result = subprocess.run([os.environ.get("TEST_BASH", "bash"),
                                     script.as_posix()], env=env,
                                    capture_output=True, text=True)
            if busy:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("still referenced", result.stderr)
                self.assertEqual((root / "sysctl.log").read_text().splitlines(),
                                 ["net.ipv4.tcp_congestion_control=cubic",
                                  "net.ipv4.tcp_congestion_control=lotspeed"])
                self.assertEqual(config.read_text(),
                                 "options lotspeed lotserver_removed=42\n")
                self.assertFalse((root / "modprobe.log").exists())
            else:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                args = (root / "modprobe.log").read_text().splitlines()
                self.assertEqual(args[0], "lotspeed")
                self.assertTrue(list(config.parent.glob("lotspeed.conf.pre-*")))
                if loaded:
                    self.assertEqual(set(args[1:]),
                                     {f"{k}={v}" for k, v in values.items()})
                    self.assertNotIn("removed", config.read_text())
                    self.assertNotIn("unexpected", config.read_text())
                    self.assertIn("lotserver_rate=40000000", config.read_text())
                else:
                    self.assertEqual(args, ["lotspeed"])
                    self.assertFalse(config.exists())

    def test_loaded_parameters_are_preserved(self):
        self.exercise()

    def test_busy_module_restores_previous_default(self):
        self.exercise(busy=True)

    def test_fresh_load_has_no_stale_options(self):
        self.exercise(loaded=False)


if __name__ == "__main__":
    unittest.main()

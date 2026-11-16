#!/usr/bin/env python3
"""Isolated installer lifecycle tests; never load modules or change host settings."""
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BUILD = runpy.run_path(str(ROOT / "tools/build_companion_installer.py"))["build"]
BASH = shutil.which("bash")
MARKER = b"__LOTSPEED_DUP_PAYLOAD__\n"
MOCK = r'''
import json, os, shutil, sys
from pathlib import Path
r = Path(os.environ['INSTALL_TEST_ROOT'])
s = json.loads((r/'state.json').read_text())
cmd, args = Path(sys.argv[0]).name, sys.argv[1:]
p = r/'sys/module/lotspeed_dup'
def done(code=0, output=''):
    (r/'state.json').write_text(json.dumps(s))
    if output: print(output)
    sys.exit(code)
if cmd == 'uname': done(output='6.1.test')
if cmd == 'stat': done(output='0:700')
if cmd == 'modinfo':
    field = args[1]
    values = {'name':'lotspeed_dup', 'version':'3.10.17-experimental', 'vermagic':'6.1.test SMP'}
    done(output='WRONG' if s['fault'] == field else values[field])
if cmd != 'bash': done(99, 'Unexpected privileged utility invocation: '+cmd)
assert args[0].endswith('/companion/dupctl') and Path(args[0]).is_file()
action = args[1]
s['calls'].append(args[1:])
if s['fault'] == action: done(7, 'Injected '+action+' failure')
if action == 'build':
    Path(args[0]).with_name('lotspeed_dup.ko').write_text('MOCK, NEVER LOAD')
elif action == 'unload':
    assert p.is_dir()
    shutil.rmtree(p)
elif action == 'load':
    assert args[2:] == ['all'] and not p.exists()
    (p/'parameters').mkdir(parents=True)
    (p/'version').write_text('3.10.17-experimental\n')
    (p/'parameters/enabled').write_text('N\n')
elif action == 'enable':
    (p/'parameters/enabled').write_text('Y\n')
elif action == 'disable':
    (p/'parameters/enabled').write_text('N\n')
elif action != 'status': done(99, 'Unknown action')
done()
'''


@unittest.skipUnless(os.name == "posix" and BASH, "Requires Linux/WSL Bash")
class InstallerTests(unittest.TestCase):
    """Check payload and failure ordering with private paths and mock controls.

    @author lyt
    @date 2026-11-17
    """

    def exercise(self, action=(), fault="", headers=True, corrupt=False, lock_link=False):
        with tempfile.TemporaryDirectory(prefix="dup-install-", dir=ROOT / "tests") as tmp:
            root = Path(tmp)
            for name in ("bin", "run", "tmp", "root"):
                (root / name).mkdir()
            params = root / "sys/module/lotspeed_dup/parameters"
            params.mkdir(parents=True)
            (params / "enabled").write_text("Y\n")
            (params.parent / "version").write_text("OLD\n")
            if headers:
                header = root / "lib/modules/6.1.test/build"
                header.mkdir(parents=True)
                (header / "Makefile").write_text("MOCK\n")
            if lock_link:
                (root / "run/lotspeed-dup-installer").symlink_to(root / "tmp", target_is_directory=True)
            for name in ("awk", "base64", "sha256sum", "tar", "mktemp", "mkdir", "rm", "flock", "gzip"):
                executable = shutil.which(name)
                self.assertIsNotNone(executable, name)
                (root / "bin" / name).symlink_to(executable)
            for name in ("bash", "uname", "stat", "modinfo", "make", "cc", "insmod", "rmmod"):
                mock = root / "bin" / name
                mock.write_text("#!" + sys.executable + "\n" + MOCK)
                mock.chmod(0o700)
            (root / "state.json").write_text(json.dumps({"calls": [], "fault": fault}))
            header, payload = BUILD().split(MARKER)
            header = header.decode()
            self.assertEqual(header.count('[[ $EUID -eq 0 ]]'), 1)
            header = header.replace('[[ $EUID -eq 0 ]]', '[[ 0 -eq 0 ]]')
            for prefix in ("/root/", "/run/", "/tmp/", "/lib/modules/", "/sys/module/"):
                header = header.replace(prefix, str(root) + prefix)
            if corrupt:
                payload = (b"A" if payload[:1] != b"A" else b"B") + payload[1:]
            script = root / "installer.sh"
            script.write_bytes(header.encode() + MARKER + payload)
            env = {"PATH": str(root / "bin"), "LC_ALL": "C", "INSTALL_TEST_ROOT": str(root)}
            result = subprocess.run([BASH, "--noprofile", "--norc", str(script), *action],
                                    env=env, cwd=root, capture_output=True, text=True, timeout=15)
            state = json.loads((root / "state.json").read_text())
            version = (params.parent / "version").read_text().strip() if params.parent.exists() else None
            enabled = (params / "enabled").read_text().strip() if params.exists() else None
            self.assertEqual(list((root / "tmp").iterdir()), [], "Control temporary files were not removed")
            return result, state["calls"], version, enabled

    def test_payload_is_deterministic_and_exact_source(self):
        body = BUILD()
        self.assertEqual(body, BUILD())
        header, encoded = body.split(MARKER)
        payload = base64.b64decode(encoded)
        self.assertIn(hashlib.sha256(payload).hexdigest().encode(), header)
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
            expected = {"companion/" + name for name in
                        ("lotspeed_dup.c", "policy.h", "Makefile", "dupctl", "README.md")}
            self.assertEqual(set(archive.getnames()), expected)
            for member in archive.getmembers():
                self.assertTrue(member.isfile())
                self.assertEqual(archive.extractfile(member).read(),
                                 (ROOT / member.name).read_bytes().replace(b"\r\n", b"\n"))
        published = ROOT / "install-lotspeed-dup-3.10.17.sh"
        if published.exists():
            self.assertEqual(published.read_bytes(), body)

    def test_bash_syntax(self):
        result = subprocess.run([BASH, "-n"], input=BUILD(), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_help_and_invalid_arguments_do_not_mutate(self):
        for args, success in ((("--help",), True), (("unknown",), False), (("install", "extra"), False)):
            with self.subTest(args=args):
                result, calls, version, enabled = self.exercise(args)
                self.assertEqual(result.returncode == 0, success, result.stderr)
                self.assertEqual((calls, version, enabled), ([], "OLD", "Y"))

    def test_pre_unload_failures_preserve_old_module(self):
        for settings in ({"headers": False}, {"corrupt": True}, {"fault": "build"},
                         {"fault": "version"}, {"fault": "vermagic"}, {"fault": "name"},
                         {"lock_link": True}):
            with self.subTest(settings=settings):
                result, calls, version, enabled = self.exercise(**settings)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(["unload"], calls)
                self.assertEqual((version, enabled), ("OLD", "Y"))

    def test_successful_replacement_order_and_defaults(self):
        result, calls, version, enabled = self.exercise()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls, [["build"], ["unload"], ["load", "all"], ["enable"]])
        self.assertEqual((version, enabled), ("3.10.17-experimental", "Y"))

    def test_later_failures_stop_without_false_success(self):
        sequence = [["build"], ["unload"], ["load", "all"], ["enable"]]
        for fault, length in (("unload", 2), ("load", 3), ("enable", 4)):
            with self.subTest(fault=fault):
                result, calls, _, _ = self.exercise(fault=fault)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, sequence[:length])
                self.assertNotIn("[OK]", result.stdout)

    def test_controls_do_not_build_or_replace(self):
        for action in ("status", "disable", "unload"):
            with self.subTest(action=action):
                result, calls, _, enabled = self.exercise((action,), headers=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(calls, [[action]])
                self.assertEqual(enabled, {"status": "Y", "disable": "N", "unload": None}[action])


if __name__ == "__main__":
    unittest.main(verbosity=2)

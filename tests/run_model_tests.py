#!/usr/bin/env python3
"""Compile and exercise the actual controller functions with a small TCP shim.

This is a deterministic logic test, not a kernel/network performance benchmark.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "lotspeed.c").read_text(encoding="utf-8")


def function(name):
    # Functions in this translation unit have column-zero closing braces.
    match = re.search(r"^static [^;{]*\b" + name + r"\([^;]*?^\}",
                      SOURCE, re.M | re.S)
    if match is None:
        raise RuntimeError(f"Production function not found: {name}")
    return match.group(0)


constants = "\n".join(line for line in SOURCE.splitlines()
                      if line.startswith("#define LOTSPEED_"))
parameters = SOURCE[SOURCE.index("static unsigned long lotserver_rate"):
                    SOURCE.index("static int param_set_rate")]
state = SOURCE[SOURCE.index("enum lotspeed_state {"):
               SOURCE.index("static const char* state_to_str")]
functions = [
    "lotspeed_rtt_inflated", "lotspeed_update_path_mode",
    "lotspeed_sample_loss", "lotspeed_reset_mux_history",
    "lotspeed_update_mux_activity", "lotspeed_update_round_model",
    "lotspeed_cwnd_event",
]
unit = "\n".join([
    (ROOT / "tests/model_shim.h").read_text(encoding="utf-8"),
    constants, parameters, state,
    (ROOT / "tests/model_adapter.h").read_text(encoding="utf-8"),
    *(function(name) for name in functions),
    (ROOT / "tests/model_cases.c").read_text(encoding="utf-8"),
])
with tempfile.TemporaryDirectory(prefix="lotspeed-tests-") as directory:
    source = Path(directory) / "model.c"
    binary = Path(directory) / ("model.exe" if os.name == "nt" else "model")
    source.write_text(unit, encoding="utf-8")
    for hz in (100, 250, 1000):
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu99",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
            "-Wno-unused-parameter", f"-DHZ={hz}",
            *shlex.split(os.environ.get("CFLAGS", "")),
            str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)

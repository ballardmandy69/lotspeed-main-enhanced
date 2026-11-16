#!/usr/bin/env python3
"""Build a deterministic source-only self-extracting Bash installer.

Author: lyt
Date: 2026-11-17
"""
import argparse
import base64
import gzip
import hashlib
import io
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parent.parent
FILES = ("lotspeed_dup.c", "policy.h", "Makefile", "dupctl", "README.md")


def build():
    archive = io.BytesIO()
    with gzip.GzipFile(fileobj=archive, mode="wb", mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w") as tar:
            for name in FILES:
                body = (ROOT / "companion" / name).read_bytes().replace(b"\r\n", b"\n")
                info = tarfile.TarInfo("companion/" + name)
                info.size = len(body)
                info.mode = 0o755 if name == "dupctl" else 0o644
                tar.addfile(info, io.BytesIO(body))
    payload = archive.getvalue()
    header = (ROOT / "companion/install-standalone.sh.in").read_text(encoding="utf-8")
    if header.count("@PAYLOAD_SHA256@") != 1 or not header.endswith("__LOTSPEED_DUP_PAYLOAD__\n"):
        raise ValueError("Installer template markers are invalid")
    header = header.replace("@PAYLOAD_SHA256@", hashlib.sha256(payload).hexdigest())
    return header.encode("utf-8") + base64.encodebytes(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    body = build()
    args.output.write_bytes(body)
    print(f"{hashlib.sha256(body).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# Pinned bootstrap; a complete local release package needs no remote source.
set -Eeuo pipefail

REPOSITORY="ballardmandy69/lotspeed-main-enhanced"
RELEASE_REF="v3.10.14"
SCRIPT_DIR=""
if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
    SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
fi

[[ ${EUID} -eq 0 ]] || {
    echo "[ERROR] Run this installer as root." >&2
    exit 1
}

if [[ -n "${SCRIPT_DIR}" && -f "${SCRIPT_DIR}/lotspeed.c" &&
      -f "${SCRIPT_DIR}/Makefile" && -f "${SCRIPT_DIR}/lotspeedctl" &&
      -f "${SCRIPT_DIR}/install.sh" ]]; then
    LOTSPEED_REF="${RELEASE_REF}" bash "${SCRIPT_DIR}/install.sh"
    exit
fi

WORK_DIR="$(mktemp -d /tmp/lotspeed-v31014.XXXXXX)"
trap 'rm -rf -- "${WORK_DIR}"' EXIT
ARCHIVE="${WORK_DIR}/release.tar.gz"
SOURCE_DIR="${WORK_DIR}/source"
mkdir -p "${SOURCE_DIR}"

if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --connect-timeout 10 \
        "https://codeload.github.com/${REPOSITORY}/tar.gz/refs/tags/${RELEASE_REF}" \
        -o "${ARCHIVE}"
elif command -v wget >/dev/null 2>&1; then
    wget -qO "${ARCHIVE}" \
        "https://codeload.github.com/${REPOSITORY}/tar.gz/refs/tags/${RELEASE_REF}"
else
    echo "[ERROR] curl or wget is required." >&2
    exit 1
fi

tar -xzf "${ARCHIVE}" -C "${SOURCE_DIR}" --strip-components=1
LOTSPEED_REF="${RELEASE_REF}" bash "${SOURCE_DIR}/install.sh"

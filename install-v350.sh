#!/usr/bin/env bash
# Immutable bootstrap installer for LotSpeed v3.5.0-enhanced.

set -Eeuo pipefail

REPOSITORY="ballardmandy69/lotspeed-main-enhanced"
RELEASE_REF="v3.5.0"
WORK_DIR="$(mktemp -d /tmp/lotspeed-v350.XXXXXX)"
ARCHIVE="${WORK_DIR}/release.tar.gz"
SOURCE_DIR="${WORK_DIR}/source"

cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

[[ ${EUID} -eq 0 ]] || {
    echo "[ERROR] Run this installer as root." >&2
    exit 1
}

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
touch "${SOURCE_DIR}/Makefile" "${SOURCE_DIR}/lotspeed.c" \
    "${SOURCE_DIR}/lotspeedctl" "${SOURCE_DIR}/install.sh"

LOTSPEED_REF="${RELEASE_REF}" bash "${SOURCE_DIR}/install.sh"

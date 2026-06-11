#!/usr/bin/env bash
#
# LotSpeed v3.5.1 enhanced installer
# Repository: https://github.com/ballardmandy69/lotspeed-main-enhanced
#
# Local checkout:
#   sudo bash install.sh
#
# Pinned remote release:
#   wget -qO- https://raw.githubusercontent.com/ballardmandy69/lotspeed-main-enhanced/main/install-v351.sh | sudo bash

set -Eeuo pipefail

GITHUB_REPO="${LOTSPEED_REPO:-ballardmandy69/lotspeed-main-enhanced}"
GITHUB_REF="${LOTSPEED_REF:-v3.5.1}"
INSTALL_DIR="${LOTSPEED_INSTALL_DIR:-/opt/lotspeed}"
MODULE_NAME="lotspeed"
VERSION="3.5.1-enhanced"
KERNEL_RELEASE="$(uname -r)"
MODULE_DEST="/lib/modules/${KERNEL_RELEASE}/kernel/net/ipv4/extra"
LEGACY_MODULE="/lib/modules/${KERNEL_RELEASE}/kernel/net/ipv4/lotspeed.ko"
SCRIPT_DIR=""

if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
    SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd || true)"
fi

info() { printf '\033[0;32m[INFO]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*"; }
fail() { printf '\033[0;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

check_root() {
    [[ ${EUID} -eq 0 ]] || fail "Run this installer as root."
}

check_system() {
    local major minor

    [[ -d "/lib/modules/${KERNEL_RELEASE}" ]] ||
        fail "Kernel module directory is missing for ${KERNEL_RELEASE}."

    major="${KERNEL_RELEASE%%.*}"
    minor="${KERNEL_RELEASE#*.}"
    minor="${minor%%.*}"
    if (( major < 4 || (major == 4 && minor < 9) )); then
        fail "Kernel 4.9 or newer is required; current kernel is ${KERNEL_RELEASE}."
    fi

    case "$(uname -m)" in
        x86_64|aarch64) ;;
        *) warn "Architecture $(uname -m) has not been widely tested." ;;
    esac
}

install_dependencies() {
    if [[ -f /etc/debian_version ]]; then
        info "Installing Debian/Ubuntu build dependencies..."
        apt-get update
        apt-get install -y gcc make "linux-headers-${KERNEL_RELEASE}" curl ca-certificates kmod iproute2
    elif [[ -f /etc/redhat-release ]]; then
        info "Installing RHEL-compatible build dependencies..."
        if command -v dnf >/dev/null 2>&1; then
            dnf install -y gcc make "kernel-devel-${KERNEL_RELEASE}" curl ca-certificates kmod iproute
        else
            yum install -y gcc make "kernel-devel-${KERNEL_RELEASE}" curl ca-certificates kmod iproute
        fi
    else
        fail "Unsupported distribution. Install gcc, make, curl, kmod, iproute2 and matching kernel headers manually."
    fi

    [[ -d "/lib/modules/${KERNEL_RELEASE}/build" ]] ||
        fail "Matching headers for ${KERNEL_RELEASE} are not available."
}

fetch_file() {
    local name="$1"
    local local_file="${SCRIPT_DIR}/${name}"
    local url="https://raw.githubusercontent.com/${GITHUB_REPO}/${GITHUB_REF}/${name}"

    if [[ -n "${SCRIPT_DIR}" && -f "${local_file}" ]]; then
        install -m 0644 "${local_file}" "${INSTALL_DIR}/${name}"
    else
        curl -fL --retry 3 --connect-timeout 10 "${url}" -o "${INSTALL_DIR}/${name}" ||
            fail "Failed to download ${url}. Push the branch first or run sudo bash install.sh from a local checkout."
    fi
}

prepare_source() {
    info "Preparing LotSpeed ${VERSION} source..."
    install -d -m 0755 "${INSTALL_DIR}"
    fetch_file lotspeed.c
    fetch_file Makefile
    fetch_file lotspeedctl
    chmod 0755 "${INSTALL_DIR}/lotspeedctl"
    touch "${INSTALL_DIR}/lotspeed.c" "${INSTALL_DIR}/Makefile" \
        "${INSTALL_DIR}/lotspeedctl"
}

build_module() {
    local built_module="${INSTALL_DIR}/lotspeed.ko"

    info "Building for kernel ${KERNEL_RELEASE}..."
    make -C "${INSTALL_DIR}" clean >/dev/null 2>&1 || true
    rm -f "${built_module}"
    make -C "${INSTALL_DIR}" KERNEL_RELEASE="${KERNEL_RELEASE}"
    [[ -f "${built_module}" ]] || fail "Build completed without producing ${built_module}."
}

validate_built_module() {
    local built_module="${INSTALL_DIR}/lotspeed.ko"
    local built_version parameters parameter
    local required_parameters=(
        lotserver_pacing_gain
        lotserver_probe_rtt_interval_ms
        lotserver_probe_rtt_duration_ms
        lotserver_probe_rtt_cwnd_pct
        lotserver_min_rtt_window_sec
        lotserver_rtt_tolerance_pct
        lotserver_min_rate_pct
        lotserver_loss_guard
        lotserver_noncong_beta
        lotserver_hd_enable
        lotserver_hd_thresh_us
        lotserver_hd_gain_boost
    )

    built_version="$(modinfo -F version "${built_module}" 2>/dev/null || true)"
    [[ "${built_version}" == "${VERSION}" ]] ||
        fail "Built module version is '${built_version:-unknown}', expected '${VERSION}'. Refusing to install a stale module."

    parameters="$(modinfo -p "${built_module}" 2>/dev/null || true)"
    for parameter in "${required_parameters[@]}"; do
        grep -q "^${parameter}:" <<<"${parameters}" ||
            fail "Built module is missing ${parameter}. Refusing to install an incomplete module."
    done

    info "Validated module ${built_version} and enhanced parameter set."
}

choose_fallback_cc() {
    local available
    available="$(sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || true)"
    for cc in cubic reno bbr; do
        if grep -qw "${cc}" <<<"${available}"; then
            printf '%s\n' "${cc}"
            return
        fi
    done
    awk '{print $1}' <<<"${available}"
}

install_module() {
    local fallback loaded_version parameter
    local required_parameters=(
        lotserver_pacing_gain
        lotserver_probe_rtt_interval_ms
        lotserver_probe_rtt_duration_ms
        lotserver_probe_rtt_cwnd_pct
        lotserver_min_rtt_window_sec
        lotserver_rtt_tolerance_pct
        lotserver_min_rate_pct
        lotserver_loss_guard
        lotserver_noncong_beta
        lotserver_hd_enable
        lotserver_hd_thresh_us
        lotserver_hd_gain_boost
    )

    if lsmod | awk '{print $1}' | grep -qx "${MODULE_NAME}"; then
        fallback="$(choose_fallback_cc)"
        [[ -n "${fallback}" ]] && sysctl -w "net.ipv4.tcp_congestion_control=${fallback}" >/dev/null
        rmmod "${MODULE_NAME}" ||
            fail "The old module is still referenced. Close existing LotSpeed TCP connections and run the installer again."
    fi

    rm -f "${LEGACY_MODULE}" "${LEGACY_MODULE}.xz" \
        "${LEGACY_MODULE}.zst" "${LEGACY_MODULE}.gz"
    install -d -m 0755 "${MODULE_DEST}"
    install -m 0644 "${INSTALL_DIR}/lotspeed.ko" "${MODULE_DEST}/lotspeed.ko"
    depmod -a
    modprobe "${MODULE_NAME}"

    grep -qw "${MODULE_NAME}" /proc/sys/net/ipv4/tcp_available_congestion_control ||
        fail "The module loaded but did not register the lotspeed congestion control."

    loaded_version="$(cat "/sys/module/${MODULE_NAME}/version" 2>/dev/null || true)"
    [[ "${loaded_version}" == "${VERSION}" ]] ||
        fail "Loaded module version is '${loaded_version:-unknown}', expected '${VERSION}'."

    for parameter in "${required_parameters[@]}"; do
        [[ -f "/sys/module/${MODULE_NAME}/parameters/${parameter}" ]] ||
            fail "Loaded module is missing ${parameter}."
    done
}

install_management() {
    install -m 0755 "${INSTALL_DIR}/lotspeedctl" /usr/local/bin/lotspeed
    printf '%s\n' "${MODULE_NAME}" > /etc/modules-load.d/lotspeed.conf

    cat > /etc/sysctl.d/99-lotspeed.conf <<'EOF'
net.ipv4.tcp_congestion_control=lotspeed
net.ipv4.tcp_no_metrics_save=1
EOF
    sysctl -p /etc/sysctl.d/99-lotspeed.conf >/dev/null
}

main() {
    check_root
    check_system
    install_dependencies
    prepare_source
    build_module
    validate_built_module
    install_module
    install_management

    printf '\nLotSpeed %s installed successfully.\n' "${VERSION}"
    printf 'Recommended preset for mixed clients in China:\n'
    printf '  lotspeed preset domestic-mixed\n'
    printf 'Check it with:\n'
    printf '  lotspeed status\n'
}

main "$@"

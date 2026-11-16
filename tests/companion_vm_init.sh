#!/bin/busybox sh
# Guest-only init. No host networking, disks, module loading or services.
# @author lyt
# @date 2026-11-17
export PATH=/bin

# Refuse an accidental interactive invocation before even mounting /proc.
if [ "$$" -ne 1 ]; then
    echo 'Refusing VM init outside PID 1' >&2
    exit 1
fi

finish() {
    echo "COMPANION_VM_RESULT=$1"
    /bin/busybox poweroff -f
    while :; do /bin/busybox sleep 1; done
}

/bin/busybox mount -t proc proc /proc || finish 1
case " $(/bin/busybox cat /proc/cmdline) " in
    *" lotspeed_dup_test=1 "*) ;;
    *) echo 'Refusing init without guest test marker'; finish 1 ;;
esac
budget_pct=10
max_mbps=10
all_ports=0
for argument in $(/bin/busybox cat /proc/cmdline); do
    case "$argument" in
        lotspeed_dup_budget_pct=*) budget_pct=${argument#lotspeed_dup_budget_pct=} ;;
        lotspeed_dup_max_mbps=*) max_mbps=${argument#lotspeed_dup_max_mbps=} ;;
        lotspeed_dup_all_ports=*) all_ports=${argument#lotspeed_dup_all_ports=} ;;
    esac
done
case "$all_ports" in
    0) port=5201; budgeted=1 ;;
    1) port=0; budgeted=0 ;;
    *) echo 'Invalid guest all_ports flag'; finish 1 ;;
esac
for number in "$budget_pct" "$max_mbps"; do
    case "$number" in ''|*[!0-9]*) echo 'Invalid guest budget/rate'; finish 1 ;; esac
done
[ "$budget_pct" -ge 1 ] && [ "$budget_pct" -le 100 ] || finish 1
[ "$max_mbps" -ge 1 ] && [ "$max_mbps" -le 1000 ] || finish 1
/bin/busybox mount -t sysfs sysfs /sys || finish 1
/bin/busybox mount -t devtmpfs devtmpfs /dev || finish 1
/bin/busybox mkdir -p /dev/net
[ -c /dev/net/tun ] || /bin/busybox mknod /dev/net/tun c 10 200 || finish 1
# Optional dependency modules are embedded in explicit load order by the runner.
for module in /modules/dep-*.ko; do
    [ -f "$module" ] || continue
    /bin/busybox insmod "$module" || finish 1
done
/bin/busybox insmod /modules/lotspeed_dup.ko port="$port" all_ports="$all_ports" delay_ms=5 \
    budgeted="$budgeted" budget_pct="$budget_pct" max_mbps="$max_mbps" || finish 1
/bin/busybox timeout -s KILL 100 /companion_vm
status=$?
# Always attempt cleanup in this disposable guest, including on assertion failure.
echo 0 > /sys/module/lotspeed_dup/parameters/enabled || status=1
/bin/busybox cat /sys/module/lotspeed_dup/parameters/stats
/bin/busybox rmmod lotspeed_dup || status=1
[ "$status" -eq 0 ] || finish 1
finish 0

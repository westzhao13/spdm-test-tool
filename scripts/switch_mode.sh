#!/usr/bin/env bash
# switch_mode.sh mctp|doe — switch SPDM transport deployment mode.
#   mctp : load cxl driver stack (VU mailbox / MCTP path, /dev/cxl/<mem>)
#   doe  : unload cxl stack, load doe.ko (/dev/doe0)
# The two modes are mutually exclusive: the device has a single PCI driver
# slot and DOE mailbox registers have no kernel/userspace arbitration.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
MODE=${1:-}
MCTP_PIDFILE=${MCTP_PIDFILE:-/tmp/spdm_mctp_bridge.pid}
DOE_PIDFILE=${DOE_PIDFILE:-/tmp/spdm_doe_fwd.pid}

usage() {
    echo "Usage: $0 mctp|doe"
    echo "  mctp : load cxl driver stack (VU/MCTP path)"
    echo "  doe  : unload cxl stack, load doe.ko (/dev/doe0)"
    exit 1
}

[ -z "$MODE" ] && usage

kill_by_pidfile() {
    local pf=$1
    if [[ -f $pf ]]; then
        kill "$(cat "$pf")" 2>/dev/null || true
        rm -f "$pf"
    fi
}

case "$MODE" in
    mctp)
        kill_by_pidfile "$DOE_PIDFILE"
        if [[ -d /sys/bus/pci/drivers/doe ]]; then
            "$SCRIPT_DIR/uninstall_doe_driver.sh" || true
        fi
        "$SCRIPT_DIR/install_cxl_driver.sh"
        echo "[switch] mode=mctp (cxl stack loaded)"
        ;;
    doe)
        kill_by_pidfile "$MCTP_PIDFILE"
        "$SCRIPT_DIR/remove_cxl_driver.sh" || true
        "$SCRIPT_DIR/install_doe_driver.sh"
        echo "[switch] mode=doe (/dev/doe0 ready)"
        ;;
    *)
        usage
        ;;
esac

echo "[switch] done. /dev/cxl: $(ls /dev/cxl/ 2>/dev/null | tr '\n' ' '); /dev/doe*: $(ls /dev/doe* 2>/dev/null | tr '\n' ' ')"

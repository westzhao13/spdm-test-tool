#!/usr/bin/bash
# Load the out-of-tree doe.ko and bind cc53 CXL devices to it.
# The module source is vendored at driver/doe/ in this repo; it is built on
# demand here. Requires kernel headers at /lib/modules/$(uname -r)/build.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRIVER_DIR="$SCRIPT_DIR/../driver/doe"
KO="$DRIVER_DIR/doe.ko"
VID_DID="cc53 1030"

if [[ $EUID -ne 0 ]]; then
	echo "run as root" >&2
	exit 1
fi

if [[ ! -d $DRIVER_DIR ]]; then
	echo "missing $DRIVER_DIR" >&2
	exit 1
fi

if [[ ! -f $KO ]]; then
	echo "no $KO — building the module" >&2
	# Build as the invoking user when we got here via sudo, so the kbuild
	# artifacts are not left root-owned in the working tree.
	if [[ -n ${SUDO_USER:-} ]]; then
		sudo -u "$SUDO_USER" make -C "$DRIVER_DIR" >&2
	else
		make -C "$DRIVER_DIR" >&2
	fi
fi

if [[ ! -f $KO ]]; then
	echo "build did not produce $KO" >&2
	exit 1
fi

mapfile -t bdfs < <(lspci -d cc53: -D | awk '{ print $1 }')
if [[ ${#bdfs[@]} -eq 0 ]]; then
	echo "no PCI device matching vendor cc53" >&2
	exit 1
fi

for bdf in "${bdfs[@]}"; do
	drv="/sys/bus/pci/devices/$bdf/driver"
	if [[ -e $drv ]]; then
		echo "$VID_DID" >"$drv/remove_id" 2>/dev/null || true
		echo "$bdf" >"$drv/unbind" 2>/dev/null || true
	fi
done

if lsmod | grep -q '^doe '; then
	rmmod doe
fi
insmod "$KO"

echo "$VID_DID" >/sys/bus/pci/drivers/doe/new_id 2>/dev/null || true
for bdf in "${bdfs[@]}"; do
	echo "$bdf" >/sys/bus/pci/drivers/doe/bind 2>/dev/null || true
done

ls -l /dev/doe*

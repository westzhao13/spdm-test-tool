#!/usr/bin/bash
# Unbind cc53 devices from doe and unload doe.ko.
set -euo pipefail

VID_DID="cc53 1030"

if [[ $EUID -ne 0 ]]; then
	echo "run as root" >&2
	exit 1
fi

if [[ ! -d /sys/bus/pci/drivers/doe ]]; then
	echo "doe driver not registered"
	exit 0
fi

# Unbind every device currently held by doe
shopt -s nullglob
for link in /sys/bus/pci/drivers/doe/*:*; do
	bdf=$(basename "$link")
	echo "$bdf" >/sys/bus/pci/drivers/doe/unbind 2>/dev/null || true
done
shopt -u nullglob

echo "$VID_DID" >/sys/bus/pci/drivers/doe/remove_id 2>/dev/null || true

if lsmod | grep -q '^doe '; then
	rmmod doe
fi

if ls /dev/doe* &>/dev/null; then
	echo "warning: /dev/doe* still present after rmmod" >&2
	ls -l /dev/doe* >&2
	exit 1
fi

# doe 解绑后设备处于无主状态；cxl_pci 模块若已加载，modprobe 是 no-op，
# 必须手动 bind 才能让 cxl 重新接管（否则 mem0/dax 不会出现）
for bdf in $(lspci -d cc53: -D | awk '{ print $1 }'); do
	if [[ -e /sys/bus/pci/drivers/cxl_pci ]] && [[ ! -e /sys/bus/pci/devices/$bdf/driver ]]; then
		echo "$bdf" >/sys/bus/pci/drivers/cxl_pci/bind 2>/dev/null \
			&& echo "$bdf rebound to cxl_pci" \
			|| echo "warning: failed to rebind $bdf to cxl_pci" >&2
	fi
done

echo "doe driver unloaded"

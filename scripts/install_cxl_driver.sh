#!/usr/bin/bash
#

USING_MSIX=${1:-1}

set -euo pipefail -x
modprobe einj
modprobe cxl_core do_not_clear_hdmdec_commit=1 hdm_commit_timeout=20 cxl_probe_by_hdmdec_only=1
modprobe dax_cxl
modprobe device_dax
modprobe kmem
modprobe dax_hmem

# 6.5+ 内核中 cxl_mem/cxl_port/cxl_pmem 已并入 cxl_core，无独立模块；老内核才有
modprobe cxl_mem  || true
modprobe cxl_port || true
modprobe cxl_pmu  || true
modprobe cxl_pmem || true

modprobe cxl_acpi
modprobe cxl_pci mbox_timeout=300 mbox_bg_timeout=300 using_msix=$USING_MSIX

cat /sys/module/cxl_core/parameters/cxl_probe_by_hdmdec_only 2>/dev/null || true
cat /sys/module/cxl_core/parameters/do_not_clear_hdmdec_commit 2>/dev/null || true
cat /sys/module/cxl_core/parameters/hdm_commit_timeout 2>/dev/null || true
cat /sys/module/cxl_pci/parameters/mbox_timeout 2>/dev/null || true
cat /sys/module/cxl_pci/parameters/mbox_bg_timeout 2>/dev/null || true
cat /sys/module/cxl_pci/parameters/using_msix 2>/dev/null || true

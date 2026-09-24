#!/usr/bin/bash
modprobe -r kmem
modprobe -r device_dax
modprobe -r dax_hmem
modprobe -r dax_cxl
modprobe -r cxl_pmem
modprobe -r cxl_pmu
modprobe -r cxl_port
modprobe -r cxl_mem
modprobe -r cxl_pci
modprobe -r cxl_acpi
modprobe -r cxl_core
modprobe -r einj

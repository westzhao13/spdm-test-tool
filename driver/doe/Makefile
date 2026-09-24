# Modify the Makefile to point to Linux build tree
DIST ?= $(shell uname -r)
# KDIR is overridable from the command line for out-of-tree kernel builds
KDIR ?= /lib/modules/$(DIST)/build/

PWD = $(shell pwd)

# Include your kernel configuration; KCONF is overridable
KCONF ?= $(if $(wildcard /boot/config-$(DIST)),/boot/config-$(DIST),$(KDIR).config)
-include $(KCONF)

VU13P_CFLAGS = -D__ZEBU__=0 -D__FPGA__=1 -D__VU13p__=1 -D__MLD__=0 -DNORMAL_DOE_CAP_OFF=0xd00 -DSECURITY_DOE_CAP_OFF=0xd80
ZEBU_CFLAGS =  -D__ZEBU__=1 -D__FPGA__=0 -D__VU13p__=0 -D__MLD__=0 -DNORMAL_DOE_CAP_OFF=0xd00 -DSECURITY_DOE_CAP_OFF=0xd80
ZEBU_MLD_CFLAGS = -D__ZEBU__=1 -D__FPGA__=0 -D__VU13p__=0 -D__MLD__=1 -DNORMAL_DOE_CAP_OFF=0xd00 -DSECURITY_DOE_CAP_OFF=0xd80

is_kvm := $(shell lscpu | grep "Hypervisor vendor" | awk '{ exit $$3 == "KVM" ? 0 : 1} END { if (NR == 0) exit 1 } '; echo $$?)
ifeq ($(is_kvm), 0)
    is_mld := $(shell lspci -d cc53::0502 | wc -l | awk '{ exit $$1 > 1 ? 0 : 1}'; echo $$?)
    ifeq ($(is_mld), 0)
        CFLAGS_MODULE = $(ZEBU_MLD_CFLAGS)
    else
        CFLAGS_MODULE = $(ZEBU_CFLAGS)
    endif
else
    CFLAGS_MODULE = $(VU13P_CFLAGS)
endif

TARGET = doe_ko

all: clean $(PREPROCESSED) $(TARGET)

MODULE_NAME=doe
$(MODULE_NAME)-objs += doe_main.o libdoe/pcie-doe.o
obj-m += $(MODULE_NAME).o

$(TARGET):
	$(MAKE) -C $(KDIR) M=$(PWD) modules CFLAGS_MODULE="$(CFLAGS_MODULE)"

SRCS := doe_main.c libdoe/pcie-doe.c
PREPROCESSED := $(SRCS:.c=.i) 
%.i: %.c
	$(MAKE) -C $(KDIR) M=$(PWD) $*.i CFLAGS_MODULE="$(CFLAGS_MODULE)"

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f $(PREPROCESSED)

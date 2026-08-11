# SPDM test tool - host-side libspdm requester.
# Links against the prebuilt libspdm + openssl bundled in ../lib/ (x86_64 Linux);
# no libspdm/openssl source needed on the build machine.
# Override LIBSPDM_INC / LIBSPDM_LIB / LIBSPDM_OPENSSL_LIB to use a local build.

SPDM_TOOL_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
LIBSPDM_INC   ?= $(abspath $(SPDM_TOOL_DIR)../lib/include)
LIBSPDM_LIB   ?= $(abspath $(SPDM_TOOL_DIR)../lib/libspdm)
LIBSPDM_OPENSSL_LIB ?= $(abspath $(SPDM_TOOL_DIR)../lib/openssl)

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -I$(LIBSPDM_INC)/spdm -I$(LIBSPDM_INC)/os_stub

OBJS = main.o spdm_client.o transport_tcp.o transport_mctp.o transport_doe.o

LIBS = -L$(LIBSPDM_LIB) \
	-lspdm_requester_lib \
	-lspdm_common_lib \
	-lspdm_secured_message_lib \
	-lspdm_crypt_lib \
	-lspdm_crypt_ext_lib \
	-lcryptlib_openssl \
	-lspdm_transport_mctp_lib \
	-lspdm_transport_pcidoe_lib \
	-lspdm_transport_tcp_lib \
	-lspdm_device_secret_lib_sample \
	-lrnglib \
	-lmalloclib \
	-lmemlib \
	-ldebuglib \
	-lplatform_lib \
	-L$(LIBSPDM_OPENSSL_LIB) -lcrypto \
	-lpthread -ldl -lm

all: spdm_tool responder

spdm_tool: $(OBJS)
	$(CC) -o $@ $(OBJS) -Wl,--start-group $(LIBS) -Wl,--end-group

responder: spdm_responder_udp.o
	$(CC) -o $@ spdm_responder_udp.o -Wl,--start-group -lspdm_responder_lib $(LIBS) -Wl,--end-group

%.o: %.c spdm_tool.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f spdm_tool responder $(OBJS) spdm_responder_udp.o

.PHONY: all clean

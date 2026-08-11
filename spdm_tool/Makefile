# SPDM test tool - host-side libspdm requester
# Links against locally built libspdm (openssl backend).

# Link against spdm-emu's submodule libspdm (same version as spdm_responder_emu,
# avoids TCP-binding wire format mismatch between libspdm 3.8.2 and newer).
LIBSPDM_DIR ?= /home/xiangzhao/pine/sfx/spdm/spdm-emu/libspdm
LIBSPDM_LIB  = /home/xiangzhao/pine/sfx/spdm/spdm-emu/build/lib

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -I$(LIBSPDM_DIR)/include -I$(LIBSPDM_DIR)/os_stub/include

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

LIBSPDM_OPENSSL_LIB ?= /home/xiangzhao/pine/sfx/spdm/spdm-emu/build/out/openssllib.out/openssl-install/lib

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

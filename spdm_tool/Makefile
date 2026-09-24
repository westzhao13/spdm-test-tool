# SPDM test tool - host-side libspdm requester.
# Deps are built from the submodules in ../lib/ and land in ../lib/build/:
#   ../lib/libspdm  — DMTF/libspdm @ 4.0.0-rc   (CMake: ARCH=x64 TOOLCHAIN=GCC CRYPTO=openssl)
#   ../lib/openssl  — openssl/openssl @ 3.5.5   (out-of-tree Configure + make build_libs)
# `make` pulls both in automatically (both binaries depend on the archives);
# `make deps` builds them on their own. `make -j` is safe.
#
# Override any LIBSPDM_*/OPENSSL_* variable to point at an existing build instead.

SPDM_TOOL_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Submodule source dir (used for include paths and for CMake invocation).
LIBSPDM_SRC  ?= $(abspath $(SPDM_TOOL_DIR)../lib/libspdm)
OPENSSL_SRC  ?= $(abspath $(SPDM_TOOL_DIR)../lib/openssl)

# Submodule build dirs (created by `make deps`).
# Kept OUTSIDE the submodules: a parent .gitignore cannot reach into a submodule,
# so building in-tree would leave lib/openssl permanently "modified". lib/build/
# is a plain directory owned by this repo, covered by .gitignore.
LIBSPDM_BUILD  ?= $(abspath $(SPDM_TOOL_DIR)../lib/build/libspdm)
OPENSSL_BUILD  ?= $(abspath $(SPDM_TOOL_DIR)../lib/build/openssl)

# Headers (submodule-tracked). libspdm 4.0.0-rc flattened its include layout —
# split across two roots, matching libspdm's own CMakeLists.txt:
#   include/         — public API (library/, industry_standard/, internal/, hal/)
#   os_stub/include/ — portable shims (library/malloclib.h, library/rnglib.h,
#                      library/spdm_crypt_ext_lib.h)
# spdm_tool uses headers from both, so both must be on the include path.
LIBSPDM_PUB_INC  ?= $(LIBSPDM_SRC)/include
LIBSPDM_OS_INC   ?= $(LIBSPDM_SRC)/os_stub/include
# Static libraries built by `make deps`.
LIBSPDM_LIB          ?= $(LIBSPDM_BUILD)/lib
LIBSPDM_OPENSSL_LIB  ?= $(OPENSSL_BUILD)

CC       ?= gcc
CFLAGS   ?= -O2 -g -Wall -Wextra
CFLAGS   += -I$(LIBSPDM_PUB_INC) -I$(LIBSPDM_OS_INC)

OBJS = main.o spdm_client.o spdm_io.o transport_tcp.o transport_mctp.o transport_doe.o

# Archives produced by `make deps`. Both binaries link them, so both depend on
# them directly — otherwise `make -j` starts linking while cmake/Configure are
# still running.
DEP_ARCHIVES = $(OPENSSL_BUILD)/libcrypto.a $(LIBSPDM_LIB)/libspdm_requester_lib.a

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

.PHONY: all deps libspdm openssl clean distclean help

all: spdm_tool responder

help:
	@echo "Targets:"
	@echo "  all       - build deps + spdm_tool + responder (default)"
	@echo "  deps      - build libspdm + openssl from submodules"
	@echo "  libspdm   - build libspdm static libs only"
	@echo "  openssl   - build openssl libcrypto.a only"
	@echo "  spdm_tool - build the host-side requester tool"
	@echo "  responder - build the bundled UDP/DOE test responder"
	@echo "  clean     - remove spdm_tool, responder, *.o (deps untouched)"
	@echo "  distclean - clean + remove lib/build/ (deps rebuilt from source)"

# --- libspdm 4.0.0-rc (CMake) ---------------------------------------------------
deps: libspdm openssl
# libspdm links against openssl's libcrypto, so libspdm target depends on
# openssl first (libcrypto.a must exist for cmake configure + libspdm link).
libspdm: $(OPENSSL_BUILD)/libcrypto.a $(LIBSPDM_LIB)/libspdm_requester_lib.a
$(LIBSPDM_LIB)/libspdm_requester_lib.a: $(OPENSSL_BUILD)/libcrypto.a
	@echo "[deps] CMake-configure libspdm (ARCH=x64 TOOLCHAIN=GCC CRYPTO=openssl ENABLE_BINARY_BUILD=1 COMPILED_LIBCRYPTO_PATH=$(OPENSSL_BUILD) COMPILED_LIBSSL_PATH=$(OPENSSL_BUILD) DISABLE_TESTS=1)"
	cd $(LIBSPDM_SRC) && \
	  cmake -B $(LIBSPDM_BUILD) -S . \
	    -DARCH=x64 -DTOOLCHAIN=GCC -DTARGET=Debug \
	    -DCRYPTO=openssl -DENABLE_BINARY_BUILD=1 \
	    -DCOMPILED_LIBCRYPTO_PATH=$(OPENSSL_BUILD) \
	    -DCOMPILED_LIBSSL_PATH=$(OPENSSL_BUILD) \
	    -DDISABLE_TESTS=1 \
	    -DDEVICE=sample
	cd $(LIBSPDM_SRC) && cmake --build $(LIBSPDM_BUILD) -j$$(nproc)

# --- openssl 3.5.5 (out-of-source Configure + make build_libs) ------------------
openssl: $(OPENSSL_BUILD)/libcrypto.a
$(OPENSSL_BUILD)/libcrypto.a:
	@echo "[deps] Configure openssl 3.5.5 out-of-source (linux-x86_64 no-shared)"
	mkdir -p $(OPENSSL_BUILD)
	cd $(OPENSSL_BUILD) && \
	  $(OPENSSL_SRC)/Configure linux-x86_64 no-shared \
	    --prefix=$(abspath $(OPENSSL_BUILD))/prefix \
	    --openssldir=$(abspath $(OPENSSL_BUILD))/prefix/ssl
	cd $(OPENSSL_BUILD) && make -j$$(nproc) build_libs

# --- spdm-tool requester / responder --------------------------------------------
spdm_tool: $(OBJS) $(DEP_ARCHIVES)
	$(CC) -o $@ $(OBJS) -Wl,--start-group $(LIBS) -Wl,--end-group

responder: spdm_responder_udp.o spdm_io.o $(DEP_ARCHIVES)
	$(CC) -o $@ spdm_responder_udp.o spdm_io.o -Wl,--start-group -lspdm_responder_lib $(LIBS) -Wl,--end-group

%.o: %.c spdm_tool.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f spdm_tool responder $(OBJS) spdm_responder_udp.o

# Remove built dependencies too (full reset; next `make` rebuilds from source).
distclean: clean
	rm -rf $(abspath $(SPDM_TOOL_DIR)../lib/build)

# spdm-test-tool

Host-side SPDM test tool (libspdm-based requester) and supporting docs/scripts
for testing a CXL device's SPDM implementation over MCTP or PCIe DOE.

## Layout

```
spdm_tool/    SPDM test tool source (libspdm requester, TCP/MCTP/DOE transports)
              + spdm_responder_udp.c (UDP DOE responder for end-to-end testing)
doc/          Design/plan/test-flow/DOE-driver-research/TCP-conversion docs
scripts/      switch_mode.sh - mctp|doe driver-mode switch (doe.ko vs cxl stack)
```

## Build

Prebuilt libspdm + openssl are bundled in `lib/` (x86_64 Linux, openssl
backend) — no libspdm/openssl source required on the build machine. Build
machine needs only gcc and standard system libs (pthread/dl/m).

```bash
cd spdm_tool
make            # spdm_tool + responder
```

> For another architecture, build libspdm yourself and point the Makefile's
> `LIBSPDM_INC` / `LIBSPDM_LIB` / `LIBSPDM_OPENSSL_LIB` at your build.

## Usage

```bash
# TCP smoke against spdm_responder_emu
spdm_tool --trans tcp --cert <root-cert.der>

# MCTP path (needs AF_MCTP kernel + mctp_bridge + bridge)
spdm_tool --trans mctp --eid 8 --cert <root-cert.der>

# DOE path (needs doe.ko + receiver)
sudo scripts/switch_mode.sh doe
cxl_test_tool -s <bdf> -U 2324 -k      # receiver (in the original CXL tool repo)
spdm_tool --trans doe --doe-udp 127.0.0.1:2324 --cert <root-cert.der>
```

> The DOE receiver (`udp_doe_forward_loop`, `-U/-C/-k` options) lives in the
> original cxl_test_tool source (`pine_vd_scripts/CXL_SCAN_TOOL/cxl_test_tool/`);
> this repo keeps the host-side tool, docs and deployment script.

## Verification status

- M1 TCP smoke: full SPDM flow PASS (GET_VERSION→MEASUREMENTS, signature verified)
- DOE end-to-end: full 7-step flow PASS against spdm_responder_udp
- Hardware smoke (Gate 0 → M2/M3/M4): pending real device environment

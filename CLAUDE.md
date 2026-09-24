# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Host-side **SPDM test tool** built on **`libspdm` `4.0.0-rc`** + **`OpenSSL` `3.5.5`**, pulled in as **git submodules** (`lib/libspdm`, `lib/openssl`) and built from source — a libspdm Requester that talks to a CXL device's SPDM responder over **TCP / MCTP (AF_MCTP) / PCIe DOE**, plus a minimal UDP/DOE `spdm_responder_udp` for end-to-end verification when no real hardware is present. It is a **verification tool**, not a production SPDM stack; the goal is to validate that a device's SPDM implementation is wire-correct and crypto-correct.

The repo is self-contained for the DOE path too: `driver/doe/` holds the vendored
out-of-tree kernel module (`doe.ko`, GPL-2.0, separate from this repo's
Apache-2.0) that provides `/dev/doeN`, and `scripts/` holds the mode scripts that
install/uninstall it.

## Build

Submodules build themselves from the Makefile (openssl via `./Configure` + `make build_libs`, libspdm via CMake). Both build dirs live under `lib/build/` — deliberately **outside** the submodules, because a parent `.gitignore` cannot reach into a submodule and an in-tree build would leave `lib/openssl` permanently reported as modified. Network access required on first build for `git submodule update --init`.

```bash
git submodule update --init --recursive   # once, after clone
cd spdm_tool
make           # deps + spdm_tool + responder; `make -j` is safe
make clean     # remove host binaries only (deps kept)
make distclean # also removes lib/build/
```

Both binaries depend on the dep archives directly, so `make -j` and a bare `make spdm_tool` order correctly on a fresh clone.

For a different arch / different libspdm build, override `LIBSPDM_SRC / LIBSPDM_BUILD / OPENSSL_SRC / OPENSSL_BUILD` in `spdm_tool/Makefile`.

## Run / smoke

```bash
# TCP smoke against libspdm's spdm_responder_emu
spdm_tool/spdm_tool --trans tcp --cert <root-cert.der>

# MCTP (needs AF_MCTP kernel + mctp_bridge + bridge.py + device SPDM-over-MCTP)
spdm_tool/spdm_tool --trans mctp --eid 8 --cert <root-cert.der>

# DOE direct (/dev/doe0 — driver loaded via scripts/switch_mode.sh doe)
sudo scripts/switch_mode.sh doe
spdm_tool/spdm_tool --trans doe --cert <root-cert.der>
spdm_tool/spdm_tool --trans doe --doe-cap security --cert <root-cert.der>   # if SPDM not on normal instance

# Pure-software loopback - no hardware, no doe.ko (see doc/doe_transport.md):
#   run spdm_tool/responder, then point the tool at it
spdm_tool/responder &                        # needs ecp384/ in cwd, see the doc
spdm_tool/spdm_tool --trans doe --doe-udp 127.0.0.1:2326 --cert <root-cert.der>
# To relay to real hardware instead, the UDP peer is a receiver process from the
# external cxl_sideband repo (https://github.com/whou-sfx/cxl_sideband)

# Real-hardware smoke (Gate 0 → M2/M3-M4, needs sudo + non-WSL2)
sudo scripts/hw_smoke.sh --cert <root.der> [--bdf bb:dd.f] [--eid 8]
```

Driver muting between cxl stack and doe.ko is mode-exclusive; the toggle script lives at `scripts/switch_mode.sh`.

## Architecture (the picture worth ~5 minutes to read)

- **CLI layer** — `spdm_tool/main.c`: getopt + switch on `--cmd` for single-step modes (`version|capabilities|algorithms|digest|cert|chal|meas`) or default full-flow.
- **libspdm init** — `spdm_tool/spdm_client.c`: `libspdm_init_context` → `libspdm_set_data` for capabilities + algorithms → `libspdm_register_transport_layer_func` → `libspdm_set_scratch_buffer`. Algorithms and capability flags are **hardcoded single-set (P-384 / SHA-384 / secp384r1 / AES-256-GCM)**; see "Pitfalls" below.
- **Transports** (each a `int tr_<x>_init(...)` registered into the same `spdm_context`):
  - `transport_tcp.c` — TCP smoke against `spdm_responder_emu`
  - `transport_mctp.c` — `AF_MCTP SOCK_DGRAM` to remote EID
  - `transport_doe.c` — two sub-modes: `--doe-udp <host:port>` (UDP relay) or `--doe-dev /dev/doeN` (direct `DOE_IOCTL_MBOX_CMD` ioctl). Self-implements **DOE Discovery** (PCI_SIG + SPDM 0x01 / Secured 0x02) before any SPDM traffic.
- **Test responder** — `spdm_tool/spdm_responder_udp.c`: libspdm responder over UDP/2326, with self-handled DOE Discovery and libspdm sample cert chain (`ecp384/bundle_responder.certchain.der`). Used for regression when no device is present.
- **Integrator hooks** — `spdm_tool/spdm_io.c`: `libspdm_read_input_file` / `libspdm_write_output_file` / `libspdm_dump_hex_str`, required by libspdm's `device_secret_lib_sample`. Shared by both binaries — do not re-declare them in `spdm_client.c` or `spdm_responder_udp.c` (that is how they got duplicated once already).
- **DOE kernel module** — `driver/doe/` (GPL-2.0, kept byte-identical to the upstream driver so it can be re-diffed; do not edit in place). Built by `make -C driver/doe`, or on demand by `scripts/install_doe_driver.sh`. `scripts/switch_mode.sh` drives install/uninstall for both modes and resolves them through `$SCRIPT_DIR`, so those four script filenames must stay exact.
- **Single-step CLI modes** dispatch into `do_<x>` in `spdm_client.c`. Full flow (default) runs `do_connection → do_digest → do_certificate → do_challenge → do_measurement` in order.

## Pitfalls / non-obvious invariants from history

These have bitten us and matter when changing anything in `spdm_client.c` / `spdm_responder_udp.c`:

- **DOE Discovery is NOT SPDM** — type `0x00` packets go through `handle_discovery` in the responder and `tr_doe_discovery` in the requester, before SPDM starts. Don't conflate.
- **SPDM 1.3+ DIGESTS RequiredSlotMask coverage** — responder's `SupportedSlotMask` (param1) must cover `ProvisionedSlotMask`. Set both via `LIBSPDM_DATA_LOCAL_SUPPORTED_SLOT_MASK` (≥ 1.3).
- **`PSK_CAP` and `MEAS_CAP` macros are composite bits** — assigning `0x400|0x800` to `psk_cap` field lands you at field value 3, which is reserved. Use the single-bit capability macros (`SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_*`), not raw `0x400` etc.
- **Cert chain must be in SPDM format** — `[length (4B BE) | root_hash | DER...]` — use `libspdm_read_responder_public_certificate_chain`. A raw concat of DER certs is invalid.
- **Transport max = `buffer - header - tail`** — under-registering causes libspdm to truncate encoded headers; over-registering allocates bogus scratch.
- **`recvfrom` before init `fromlen`** — uninitialised `fromlen` accepts the first packet as 0-byte and silently rejects later ones.
- **DOE capability offsets exist only as `-D` flags** — `NORMAL_DOE_CAP_OFF` (0xd00) / `SECURITY_DOE_CAP_OFF` (0xd80) are defined in `driver/doe/Makefile`, in no header, and `spdm_tool/transport_doe.c` duplicates the same values locally. If you change one, change both, or the tool addresses the wrong mailbox instance.
- **The DOE ioctl response starts at buffer word 0** — `doe_main.c` copies the response from a freshly allocated buffer to the caller's buffer at offset 0, overwriting the cap-offset selector the caller wrote at word 0. So the response DOE header is DW0 and its length is `DW1[17:0]`; `transport_doe.c` reading the length from `g_dev_buf + 4` is correct, not an off-by-one.
- **`KEY_EX_CAP` is set in `spdm_client.c` cap_flags but `do_key_exchange` does not exist** — the tool currently exposes only the 6-step connection+identity+measurement path; this is the central gap (Session, PSK, CSR/SET_CERT, Event, REQ_ASYM_SIGN, Multi-key, Vendor, Mut-Auth).

## Docs to read first

| File | When |
|---|---|
| `README.md` | Build & run quickstart (already complete with the major flows) |
| `doc/doe_transport.md` | How to run the DOE transport — loopback with no hardware, direct `doe.ko`, or UDP relay; includes the per-errno troubleshooting table |
| `doc/spdm_1.2_1.3_coverage_spec.md` | Active roadmap for filling the 1.2/1.3 command-surface gap — consult before adding any new `--cmd` |
| `doc/test_flow.md` | **Describes an external repo** ([cxl_sideband](https://github.com/whou-sfx/cxl_sideband)): the MCTP bridge and UDP receiver process that the MCTP path needs. Read only when working on that path |

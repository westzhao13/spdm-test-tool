# spdm-test-tool

Host-side SPDM test tool (libspdm-based requester) and supporting docs/scripts
for testing a CXL device's SPDM implementation over MCTP or PCIe DOE.

## Layout

```
spdm_tool/    SPDM test tool source (libspdm requester, TCP/MCTP/DOE transports)
              + spdm_responder_udp.c (UDP DOE responder for end-to-end testing)
              + spdm_io.c (integrator file-I/O hooks shared by both binaries)
driver/doe/   out-of-tree PCIe DOE kernel module (doe.ko) — GPL-2.0, vendored
lib/          libspdm + openssl git submodules; lib/build/ holds their build output
doc/          test_flow.md (transport/wire detail), spdm_1.2_1.3_coverage_spec.md
scripts/      switch_mode.sh (mctp|doe driver switch) + the mode scripts it drives,
              hw_smoke.sh (on-device smoke)
```

## DOE kernel driver (`driver/doe/`)

The `--trans doe --doe-dev /dev/doeN` path needs the out-of-tree `doe.ko`. Its
source is vendored here so the repo is self-contained.

- **License**: GPL-2.0 (`driver/doe/LICENSE`) — separate from this repo's
  Apache-2.0. It is a kernel module, so that split is expected.
- **Vendored from** `pine_vd_scripts@9a78266` (`cxl_tools/doe_test_app/driver/`),
  copied verbatim. Edits made there do not propagate — re-diff against upstream
  if you need to pull a change.
- **Build**: `make -C driver/doe` (also done on demand by
  `scripts/install_doe_driver.sh`). Needs kernel headers at
  `/lib/modules/$(uname -r)/build`; override with `KDIR=` / `DIST=`. Note the
  default goal cleans first, so every `make` is a full rebuild.
- The module binds by **PCI class `0x050210`**; the `cc53 1030` id is injected by
  the install script via sysfs `new_id`.
- Host auto-detection (`lscpu` hypervisor probe + `lspci -d cc53::0502 | wc -l`)
  selects the VU13P / ZEBU / ZEBU-MLD build variant. All three carry the same DOE
  capability offsets (`0xd00` normal, `0xd80` security) — defined only as `-D`
  flags in `driver/doe/Makefile`, in no header.

`scripts/switch_mode.sh doe` installs it (and `mctp` uninstalls it); both modes
need those scripts, which is why all four live in `scripts/`.

## Pinned library versions

**Built from source as git submodules (not prebuilt binaries):**

| Library | Submodule path | Pin | Build system |
|---|---|---|---|
| **libspdm** | `lib/libspdm` | tag **`4.0.0-rc`** (commit `8a92317f`) — `LIBSPDM_MAJOR.MINOR.PATCH = 0x04.0x00.0x00` | CMake → `lib/build/libspdm/` |
| **OpenSSL** (libcrypto) | `lib/openssl` | tag **`openssl-3.5.5`** (commit `67b5686b`) — `OpenSSL 3.5.5 27 Jan 2026` | `./Configure` + `make build_libs` → `lib/build/openssl/` |

> Note: there is **no GA `4.0.0` tag** at github.com/DMTF/libspdm — only
> `4.0.0-rc`. Its version macro matched the bundle we replaced, so we pin the RC
> directly. If you need to switch versions, override `LIBSPDM_SRC` /
> `OPENSSL_SRC` in the Makefile.
>
> Both build dirs live under `lib/build/`, deliberately **outside** the
> submodules — a parent `.gitignore` cannot reach into a submodule, so an
> in-tree build would leave `lib/openssl` permanently reported as modified.

## Build

Build machine needs gcc, cmake, perl (for openssl Configure), and standard
system libs (pthread/dl/m). Submodule sources are version-pinned below; once
initialized they build themselves from the Makefile.

```bash
# 1. After cloning, initialize submodules once:
git submodule update --init --recursive

# 2. Build dependencies (libspdm + openssl from source), then the host tool:
cd spdm_tool
make            # builds deps + spdm_tool + responder; `make -j` works
```

`make` builds, in dependency order:
1. **openssl** — out-of-tree Configure (`linux-x86_64 no-shared`) + `make build_libs` → `lib/build/openssl/libcrypto.a`.
2. **libspdm** — CMake (`ARCH=x64 TOOLCHAIN=GCC TARGET=Debug CRYPTO=openssl ENABLE_BINARY_BUILD=1 DISABLE_TESTS=1`), pointed at `lib/build/openssl` via `COMPILED_LIBCRYPTO_PATH`/`COMPILED_LIBSSL_PATH` → `lib/build/libspdm/lib/*.a`.
3. **spdm_tool** (host requester) and **responder** (UDP/DOE test responder), both linking those archives.

Build outputs:
- `lib/build/libspdm/lib/*.a`, `lib/build/openssl/libcrypto.a` — gitignored
- `spdm_tool/spdm_tool`, `spdm_tool/responder` — gitignored

Other targets: `make deps` (deps only), `make libspdm` / `make openssl`
(one dep), `make clean` (host binaries only), `make distclean` (also removes
`lib/build/`), `make help`.

> To override (e.g., reuse an existing openssl / libspdm build), override
> `LIBSPDM_SRC` / `LIBSPDM_BUILD` / `OPENSSL_SRC` / `OPENSSL_BUILD` in
> `spdm_tool/Makefile`; the dep targets then no-op if the archives already exist.

## Usage

```bash
# TCP smoke against spdm_responder_emu
spdm_tool --trans tcp --cert <root-cert.der>

# MCTP path (needs AF_MCTP kernel + mctp_bridge + bridge)
spdm_tool --trans mctp --eid 8 --cert <root-cert.der>

# DOE path - direct (doe.ko loaded, no receiver needed)
sudo scripts/switch_mode.sh doe
spdm_tool --trans doe --cert <root-cert.der>     # direct, --doe-dev defaults to /dev/doe0
#   SPDM not advertised on that instance? retry with --doe-cap security

# DOE path - UDP relay (kept for regression vs spdm_responder_udp)
cxl_test_tool -s <bdf> -U 2324 -k      # receiver (in the original CXL tool repo)
spdm_tool --trans doe --doe-udp 127.0.0.1:2324 --cert <root-cert.der>
```

> The DOE receiver (`udp_doe_forward_loop`, `-U/-C/-k` options) lives in the
> original cxl_test_tool source (`pine_vd_scripts/CXL_SCAN_TOOL/cxl_test_tool/`);
> this repo keeps the host-side tool, docs and deployment script.

## Verification status

- M1 TCP smoke: full SPDM flow PASS (GET_VERSION→MEASUREMENTS, signature verified)
- DOE end-to-end: full 7-step flow PASS against spdm_responder_udp
- DOE direct (/dev/doeN ioctl via doe.ko): error paths verified locally, hardware flow pending
- Hardware smoke (Gate 0 → M2/M3/M4): pending real device environment

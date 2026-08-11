#!/usr/bin/env bash
# hw_smoke.sh - real-hardware SPDM smoke:
#   Gate 0 (device discovery) -> M2 (MCTP path) / M3-M4 (doe.ko + device)
# Usage: sudo ./hw_smoke.sh --cert <root.der> [--bdf <bb:dd.f>] [--eid <n>]
# Prereq: spdm_tool built, DOE receiver (cxl_test_tool -U) on PATH, MCTP bridge running if testing MCTP.
set -u

CERT=""; BDF=""; EID=8
while [ $# -gt 0 ]; do
  case "$1" in
    --cert) CERT="$2"; shift 2 ;;
    --bdf)  BDF="$2";  shift 2 ;;
    --eid)  EID="$2";  shift 2 ;;
    *) echo "usage: $0 --cert <root.der> [--bdf <bb:dd.f>] [--eid <n>]"; exit 1 ;;
  esac
done
[ -n "$CERT" ] || { echo "usage: $0 --cert <root.der>"; exit 1; }

SPDM_TOOL="$(cd "$(dirname "$0")/.." && pwd)/spdm_tool/spdm_tool"
RESULTS=()
pass() { RESULTS+=("PASS  $1"); echo "PASS  $1"; }
fail() { RESULTS+=("FAIL  $1"); echo "FAIL  $1"; }
skip() { RESULTS+=("SKIP  $1"); echo "SKIP  $1"; }

# --- preflight: real environment? ---
grep -qi microsoft /proc/version && { echo "ERROR: WSL2 detected - no PCI/MCTP/doe access"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "ERROR: need root (sudo)"; exit 2; }

# Gate 0: device discovery
if [ -n "$BDF" ] && lspci -s "$BDF" >/dev/null 2>&1; then
  pass "Gate 0 device at $BDF: $(lspci -s "$BDF" | cut -d' ' -f2-)"
elif command -v cxl >/dev/null 2>&1 && [ -n "$(cxl list -M 2>/dev/null)" ]; then
  pass "Gate 0 memdev via cxl list"
else
  fail "Gate 0 no CXL device (need --bdf or cxl tool)"
fi

# M2: MCTP path (AF_MCTP kernel + mctp_bridge + device SPDM-over-MCTP)
if [ -d /sys/bus/mctp ] && [ -n "$(ls /sys/bus/mctp/devices 2>/dev/null)" ]; then
  if "$SPDM_TOOL" --trans mctp --eid "$EID" --cert "$CERT" >/dev/null 2>&1; then
    pass "M2 MCTP flow (EID $EID)"
  else
    fail "M2 MCTP flow (EID $EID)"
  fi
else
  skip "M2 no AF_MCTP/device (bridge up?)"
fi

# M3: doe.ko + DOE receiver
DOE_RX_PID=""
if lsmod 2>/dev/null | grep -q '^doe' || modprobe doe 2>/dev/null; then
  if command -v cxl_test_tool >/dev/null 2>&1 && [ -n "$BDF" ]; then
    cxl_test_tool -s "$BDF" -U 2324 -k >/dev/null 2>&1 &
    DOE_RX_PID=$!
    sleep 1
    pass "M3 doe.ko + receiver :2324 (pid $DOE_RX_PID)"
  else
    fail "M3 doe.ko loaded but cxl_test_tool missing or --bdf not given"
  fi
else
  skip "M3 doe.ko unavailable"
fi

# M4: DOE path (needs M3 receiver up)
if [ -n "$DOE_RX_PID" ]; then
  if "$SPDM_TOOL" --trans doe --doe-udp 127.0.0.1:2324 --cert "$CERT" >/dev/null 2>&1; then
    pass "M4 DOE flow"
  else
    fail "M4 DOE flow"
  fi
  kill "$DOE_RX_PID" 2>/dev/null
else
  skip "M4 skipped (no DOE receiver)"
fi

echo "--- summary ---"
printf '%s\n' "${RESULTS[@]}"

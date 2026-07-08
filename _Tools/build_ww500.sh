#!/bin/bash
# Build ww500_md inside WSL with the Arm GNU 14.3.rel1 toolchain.
# Optional arg 1: the CIS model (camera variant), e.g. cis_imx708 (RP3, default)
# or cis_hm0360 (HM0360 night/IR). A clean build is forced so switching
# variants never mixes object files.
# Usage: wsl.exe -d Ubuntu-24.04 bash ./build_ww500.sh [cis_model]
set -e
CIS_MODEL="${1:-}"
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../EPII_CM55M_APP_S"
make clean > /dev/null 2>&1 || true
if [ -n "$CIS_MODEL" ]; then
  echo "=== Building variant CIS_SUPPORT_INAPP_MODEL=$CIS_MODEL ==="
  make -j8 APP_TYPE=ww500_md CIS_SUPPORT_INAPP_MODEL="$CIS_MODEL" 2>&1 | tail -20
else
  echo "=== Building default variant (from makefile) ==="
  make -j8 APP_TYPE=ww500_md 2>&1 | tail -20
fi

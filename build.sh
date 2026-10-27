#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="openwrt/sdk:ramips-mt76x8-23.05.5"

echo "=== Building mac_se30_link.ko using Docker (${IMAGE}) ==="

docker run --platform linux/amd64 --rm \
    -v "${SCRIPT_DIR}:/workspace" \
    -w /workspace \
    "${IMAGE}" \
    bash -c '
        export STAGING_DIR=/builder/staging_dir
        export PATH=/builder/staging_dir/toolchain-mipsel_24kc_gcc-12.3.0_musl/bin:/builder/staging_dir/host/bin:$PATH
        make -C /builder/build_dir/target-mipsel_24kc_musl/linux-ramips_mt76x8/linux-5.15.* \
            ARCH=mips \
            CROSS_COMPILE=mipsel-openwrt-linux-musl- \
            M=/workspace \
            modules
    '

echo ""
echo "=== Build Successful ==="
ls -lh "${SCRIPT_DIR}/mac_se30_link.ko"
file "${SCRIPT_DIR}/mac_se30_link.ko" 2>/dev/null || true

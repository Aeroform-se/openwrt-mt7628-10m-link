#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    echo "Usage: $0 [ROUTER_IP] [ROUTER_USER]"
    echo "  ROUTER_IP:   Target router IP address (default: 192.168.1.1)"
    echo "  ROUTER_USER: SSH username (default: root)"
    echo ""
    echo "Example:"
    echo "  $0                 # Deploys to root@192.168.1.1"
    echo "  $0 192.168.1.100   # Deploys to root@192.168.1.100"
    exit 0
fi

ROUTER_IP="${1:-192.168.1.1}"
ROUTER_USER="${2:-root}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_NAME="mac_se30_link.ko"

if [ ! -f "${SCRIPT_DIR}/${MODULE_NAME}" ]; then
    echo "Module not built yet. Running build.sh first..."
    "${SCRIPT_DIR}/build.sh"
fi

echo "=== Target Router: ${ROUTER_USER}@${ROUTER_IP} ==="
echo "1. Transferring ${MODULE_NAME} to /tmp and /etc on router..."

scp -O -o StrictHostKeyChecking=no "${SCRIPT_DIR}/${MODULE_NAME}" "${ROUTER_USER}@${ROUTER_IP}:/tmp/" 2>/dev/null || \
    scp -o StrictHostKeyChecking=no "${SCRIPT_DIR}/${MODULE_NAME}" "${ROUTER_USER}@${ROUTER_IP}:/tmp/"

echo "2. Installing to /etc/mac_se30_link.ko..."
ssh -o StrictHostKeyChecking=no "${ROUTER_USER}@${ROUTER_IP}" "
    cp /tmp/${MODULE_NAME} /etc/${MODULE_NAME}
    if ! grep -q '${MODULE_NAME}' /etc/rc.local; then
        sed -i 's/^exit 0/insmod \/etc\/${MODULE_NAME} 2>\/dev\/null || true\nexit 0/' /etc/rc.local
    fi
"

echo "3. Loading/reloading module via insmod..."
ssh -o StrictHostKeyChecking=no "${ROUTER_USER}@${ROUTER_IP}" "
    rmmod ${MODULE_NAME%.ko} 2>/dev/null || true
    insmod /etc/${MODULE_NAME}
"

echo ""
echo "4. Reading kernel log (dmesg) for link override results:"
echo "--------------------------------------------------------"
ssh -o StrictHostKeyChecking=no "${ROUTER_USER}@${ROUTER_IP}" "dmesg | grep 'mac_se30_link' | tail -n 15"
echo "--------------------------------------------------------"

echo ""
echo "5. Checking current switch link status:"
ssh -o StrictHostKeyChecking=no "${ROUTER_USER}@${ROUTER_IP}" "swconfig dev switch0 port 4 get link"

echo ""
echo "=== Done! Persistent 10M Half-Duplex enforcer active ==="

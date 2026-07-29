#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODULE_NAME="runtime_monitor"
MODULE_SRC="${PROJECT_ROOT}/modules/runtime_monitor/${MODULE_NAME}.ko"

TOOLS_DIR="${PROJECT_ROOT}/tools"
JPM_HOST="${TOOLS_DIR}/jpm"
JPM_AARCH64="${TOOLS_DIR}/jpm_aarch64"

KERNEL_RELEASE="$(uname -r)"
HOST_ARCH="$(uname -m)"

MODULE_INSTALL_DIR="/lib/modules/${KERNEL_RELEASE}/extra"
MODULE_INSTALL_PATH="${MODULE_INSTALL_DIR}/${MODULE_NAME}.ko"
BIN_INSTALL_PATH="/usr/local/bin/jpm"

echo "[install] project root: ${PROJECT_ROOT}"
echo "[install] host arch:    ${HOST_ARCH}"
echo "[install] kernel:       ${KERNEL_RELEASE}"

if [[ "${EUID}" -ne 0 ]]; then
    echo "[install] ERROR: please run with sudo:"
    echo "  sudo ./scripts/install.sh"
    exit 1
fi

if [[ ! -f "${MODULE_SRC}" ]]; then
    echo "[install] ERROR: kernel module not found: ${MODULE_SRC}"
    echo "Run ./scripts/build.sh first."
    exit 1
fi

select_jpm_binary() {
    if [[ "${HOST_ARCH}" == "aarch64" || "${HOST_ARCH}" == "arm64" ]]; then
        if [[ -x "${JPM_AARCH64}" ]]; then
            echo "${JPM_AARCH64}"
            return 0
        fi
        if [[ -x "${JPM_HOST}" ]]; then
            echo "${JPM_HOST}"
            return 0
        fi
    else
        if [[ -x "${JPM_HOST}" ]]; then
            echo "${JPM_HOST}"
            return 0
        fi
    fi

    return 1
}

if ! JPM_SRC="$(select_jpm_binary)"; then
    echo "[install] ERROR: jpm binary not found."
    echo "Expected one of:"
    echo "  ${JPM_HOST}"
    echo "  ${JPM_AARCH64}"
    echo "Run ./scripts/build.sh first."
    exit 1
fi

echo "[install] selected jpm binary: ${JPM_SRC}"

echo "[install] installing kernel module"
mkdir -p "${MODULE_INSTALL_DIR}"
install -m 0644 "${MODULE_SRC}" "${MODULE_INSTALL_PATH}"

echo "[install] running depmod"
depmod -a "${KERNEL_RELEASE}"

echo "[install] installing jpm CLI"
install -m 0755 "${JPM_SRC}" "${BIN_INSTALL_PATH}"

echo "[install] installed files:"
ls -lh "${MODULE_INSTALL_PATH}"
ls -lh "${BIN_INSTALL_PATH}"

echo
echo "[install] done"
echo
echo "You can now run:"
echo "  jpm monitor --duration 10 --output log.csv"
echo
echo "If your Jetson has an unsafe I2C bus, use:"
echo "  jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20"

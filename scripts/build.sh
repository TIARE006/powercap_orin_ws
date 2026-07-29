#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_DIR="${PROJECT_ROOT}/modules/runtime_monitor"
TOOLS_DIR="${PROJECT_ROOT}/tools"

MODULE_NAME="runtime_monitor"

JPM_SRC="${TOOLS_DIR}/jpm.cpp"
JPM_BIN_HOST="${TOOLS_DIR}/jpm"
JPM_BIN_AARCH64="${TOOLS_DIR}/jpm_aarch64"

HOST_ARCH="$(uname -m)"
KERNEL_RELEASE="${KERNEL_RELEASE:-$(uname -r)}"

PROJECT_KDIR="${PROJECT_ROOT}/l4t/Linux_for_Tegra/source/kernel/kernel-jammy-src"

echo "[build] project root: ${PROJECT_ROOT}"
echo "[build] module dir:   ${MODULE_DIR}"
echo "[build] tools dir:    ${TOOLS_DIR}"
echo "[build] host arch:    ${HOST_ARCH}"
echo "[build] host kernel:  $(uname -r)"

if [[ ! -d "${MODULE_DIR}" ]]; then
    echo "[build] ERROR: module directory not found: ${MODULE_DIR}"
    exit 1
fi

if [[ ! -f "${MODULE_DIR}/${MODULE_NAME}.c" ]]; then
    echo "[build] ERROR: source file not found: ${MODULE_DIR}/${MODULE_NAME}.c"
    exit 1
fi

if [[ ! -f "${JPM_SRC}" ]]; then
    echo "[build] ERROR: jpm source not found: ${JPM_SRC}"
    exit 1
fi

detect_kdir() {
    if [[ -n "${KDIR:-}" ]]; then
        echo "${KDIR}"
        return 0
    fi

    if [[ "${HOST_ARCH}" == "x86_64" || "${HOST_ARCH}" == "amd64" ]]; then
        if [[ -d "${PROJECT_KDIR}" ]]; then
            echo "${PROJECT_KDIR}"
            return 0
        fi
    fi

    local candidates=(
        "/lib/modules/${KERNEL_RELEASE}/build"
        "/usr/src/linux-headers-${KERNEL_RELEASE}"
        "/usr/src/linux-${KERNEL_RELEASE}"
    )

    for d in "${candidates[@]}"; do
        if [[ -d "$d" ]]; then
            echo "$d"
            return 0
        fi
    done

    return 1
}

build_kernel_module() {
    local kdir_resolved

    if ! kdir_resolved="$(detect_kdir)"; then
        echo "[build] ERROR: could not find kernel build directory."
        echo "For x86 cross-build, set KDIR manually if needed:"
        echo "  KDIR=/path/to/kernel/source ./scripts/build.sh"
        exit 1
    fi

    echo "[build] KDIR:         ${kdir_resolved}"

    if [[ ! -f "${kdir_resolved}/Makefile" ]]; then
        echo "[build] ERROR: KDIR does not contain a Makefile: ${kdir_resolved}"
        exit 1
    fi

    cd "${MODULE_DIR}"

    echo "[build] cleaning old kernel module artifacts"
    make clean KDIR="${kdir_resolved}" || true

    if [[ "${HOST_ARCH}" == "aarch64" || "${HOST_ARCH}" == "arm64" ]]; then
        echo "[build] native Jetson ARM64 kernel-module build"
        make KDIR="${kdir_resolved}" ARCH=arm64
    else
        echo "[build] x86 host detected; cross-compiling kernel module for Jetson ARM64"

        if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
            echo "[build] ERROR: aarch64-linux-gnu-gcc not found."
            echo "Install it with:"
            echo "  sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu"
            exit 1
        fi

        make KDIR="${kdir_resolved}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-tegra
    fi

    if [[ ! -f "${MODULE_NAME}.ko" ]]; then
        echo "[build] ERROR: ${MODULE_NAME}.ko was not created"
        exit 1
    fi

    echo
    echo "[build] kernel module success:"
    ls -lh "${MODULE_NAME}.ko"
    modinfo "${MODULE_NAME}.ko" | head -n 20 || true
}

build_host_jpm() {
    echo
    echo "[build] building host jpm"

    if ! command -v g++ >/dev/null 2>&1; then
        echo "[build] ERROR: host g++ not found."
        echo "Install it with:"
        echo "  sudo apt install -y g++"
        exit 1
    fi

    g++ -O2 -std=c++17 -Wall -Wextra \
        -o "${JPM_BIN_HOST}" \
        "${JPM_SRC}"

    echo "[build] host jpm success:"
    ls -lh "${JPM_BIN_HOST}"
    file "${JPM_BIN_HOST}" || true
}

build_aarch64_jpm() {
    echo
    echo "[build] building Jetson ARM64 jpm"

    if [[ "${HOST_ARCH}" == "aarch64" || "${HOST_ARCH}" == "arm64" ]]; then
        cp -f "${JPM_BIN_HOST}" "${JPM_BIN_AARCH64}"
        chmod +x "${JPM_BIN_AARCH64}"
    else
        if ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
            echo "[build] ERROR: aarch64-linux-gnu-g++ not found."
            echo "Install it with:"
            echo "  sudo apt install -y g++-aarch64-linux-gnu"
            exit 1
        fi

        aarch64-linux-gnu-g++ -O2 -std=c++17 -Wall -Wextra \
            -o "${JPM_BIN_AARCH64}" \
            "${JPM_SRC}"
    fi

    echo "[build] Jetson ARM64 jpm success:"
    ls -lh "${JPM_BIN_AARCH64}"
    file "${JPM_BIN_AARCH64}" || true
}

build_kernel_module
build_host_jpm
build_aarch64_jpm

echo
echo "[build] all done"
echo "[build] outputs:"
echo "  ${MODULE_DIR}/${MODULE_NAME}.ko"
echo "  ${JPM_BIN_HOST}"
echo "  ${JPM_BIN_AARCH64}"

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM_CMAKE="${ROOT}/vm"
TOOLCHAIN_X64="${VM_CMAKE}/cmake/Toolchain-x64.cmake"
TMP="${TMPDIR:-/tmp}"
BUILD_DIR="${TMP}/jello_ch11_x64"

HOST_ARCH="$(uname -m)"
HOST_OS="$(uname -s)"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cmake -S "${VM_CMAKE}" -B "${BUILD_DIR}" -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_X64}" -DJELLO_BUILD_TESTS=ON >/dev/null
cmake --build "${BUILD_DIR}" --target jit_reload_tests

RELOAD_BIN="$(find "${BUILD_DIR}" -type f -name jit_reload_tests | head -n1)"
if [[ -z "${RELOAD_BIN}" ]]; then
  echo "chapter16_jit_reload_x64: build did not produce jit_reload_tests" >&2
  exit 1
fi

arch -x86_64 "${RELOAD_BIN}"
echo "chapter16_jit_reload_x64: jit_reload_tests passed"
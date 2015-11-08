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
cmake --build "${BUILD_DIR}" --target jit_numeric_tests

NUM_BIN="$(find "${BUILD_DIR}" -type f -name jit_numeric_tests | head -n1)"
if [[ -z "${NUM_BIN}" ]]; then
  echo "chapter11_jit_numeric_x64: build did not produce jit_numeric_tests" >&2
  exit 1
fi

arch -x86_64 "${NUM_BIN}"
echo "chapter11_jit_numeric_x64: jit_numeric_tests passed"
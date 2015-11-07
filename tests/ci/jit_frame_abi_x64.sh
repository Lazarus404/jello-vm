#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Jahred Love. All rights reserved.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM_CMAKE="${ROOT}/vm"
TOOLCHAIN_X64="${VM_CMAKE}/cmake/Toolchain-x64.cmake"
TMP="${TMPDIR:-/tmp}"
BUILD_DIR="${TMP}/jello_ch10_x64"

HOST_ARCH="$(uname -m)"
HOST_OS="$(uname -s)"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cmake -S "${VM_CMAKE}" -B "${BUILD_DIR}" -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_X64}" -DJELLO_BUILD_TESTS=ON >/dev/null
cmake --build "${BUILD_DIR}" --target jit_frame_abi_tests

BIN="$(find "${BUILD_DIR}" -type f -name jit_frame_abi_tests | head -n1)"
if [[ -z "${BIN}" ]]; then
  echo "chapter10_jit_frame_abi_x64: build did not produce a jit_frame_abi_tests binary" >&2
  exit 1
fi

arch -x86_64 "${BIN}"
echo "chapter10_jit_frame_abi_x64: x64 cross-compiled JIT frame ABI test passed"

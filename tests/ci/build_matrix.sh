#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM_CMAKE="${ROOT}/vm"
TOOLCHAIN_X64="${VM_CMAKE}/cmake/Toolchain-x64.cmake"
TMP="${TMPDIR:-/tmp}"

build_one() {
  local label="$1"
  shift
  local build_dir="${TMP}/jello_${label}"
  rm -rf "${build_dir}"
  mkdir -p "${build_dir}"
  echo "== build_matrix: ${label} =="
  (cd "${build_dir}" && cmake "$@" "${VM_CMAKE}")
  cmake --build "${build_dir}" --target jello_vm
}

build_one native
build_one x64_cross -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_X64}"

echo "build_matrix: all builds passed"

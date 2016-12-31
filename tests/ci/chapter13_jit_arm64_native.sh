#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${1:?build dir required}"
JLO_TOOL="${2:-}"

HOST_ARCH="$(uname -m)"
if [[ "${HOST_ARCH}" != "arm64" && "${HOST_ARCH}" != "aarch64" ]]; then
  echo "chapter13_jit_arm64_native: SKIPPED (host is not ARM64 -- no first-class ARM64 execution layer exists on x86_64 hosts)"
  exit 0
fi

run_bin() {
  local name="$1"
  local path
  path="$(find "${BUILD_DIR}" -type f -name "${name}" -perm +111 2>/dev/null | head -n1)"
  if [[ -z "${path}" ]]; then
    echo "chapter13_jit_arm64_native: missing ${name} under ${BUILD_DIR}" >&2
    exit 1
  fi
  "${path}"
}

echo "== chapter13 native ARM64 execution bundle =="
run_bin jit_frame_abi_tests
run_bin jit_numeric_tests
run_bin jit_cfg_tests
run_bin jit_calls_tests
run_bin chapter13_jit_arm64_tests
run_bin chapter13_jit_golden_tests

if [[ -n "${JLO_TOOL}" && -x "${JLO_TOOL}" ]]; then
  "${ROOT}/tests/ci/gc_fixtures.sh" "${JLO_TOOL}"
  "${ROOT}/tests/ci/dyn_fixtures.sh" "${JLO_TOOL}"
fi

echo "chapter13_jit_arm64_native: full native ARM64 bundle passed"

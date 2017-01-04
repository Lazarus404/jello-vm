#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GEN="${ROOT}/tests/ci/gen_stress_fixtures.py"
HOST_ARCH="$(uname -m)"
HOST_OS="$(uname -s)"

JLO_TOOL=""
if [[ -x "${ROOT}/build-x64/jlo_tool" ]]; then
  JLO_TOOL="${ROOT}/build-x64/jlo_tool"
else
  VM_CMAKE="${ROOT}/vm"
  TOOLCHAIN_X64="${VM_CMAKE}/cmake/Toolchain-x64.cmake"
  BUILD_DIR="${TMPDIR:-/tmp}/jello_ch18_x64"
  cmake -S "${VM_CMAKE}" -B "${BUILD_DIR}" -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_X64}" -DJELLO_BUILD_TESTS=ON >/dev/null
  cmake --build "${BUILD_DIR}" --target jlo_tool
  JLO_TOOL="$(find "${BUILD_DIR}" -type f -name jlo_tool | head -n1)"
fi
if [[ -z "${JLO_TOOL}" || ! -x "${JLO_TOOL}" ]]; then
  echo "chapter18_stress_reg_pressure_x64: no x64 jlo_tool" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
python3 "${GEN}" "${TMP}/gen"

failures=0
fail() {
  echo "stress_reg_pressure_x64 FAIL: $*" >&2
  failures=$((failures + 1))
}

run_pair() {
  local name="$1"
  local want="$2"
  local src="${TMP}/gen/${name}.jlo.txt"
  local bin="${TMP}/${name}.jlo"
  if ! arch -x86_64 "${JLO_TOOL}" asm "${src}" -o "${bin}" 2>"${TMP}/${name}.asm.err"; then
    fail "${name}: asm failed -- $(cat "${TMP}/${name}.asm.err")"
    return
  fi
  local interp jit
  interp="$(arch -x86_64 env JELLO_JIT_DISABLE=1 "${JLO_TOOL}" run "${bin}" 2>"${TMP}/${name}.i.err" | head -1 || true)"
  jit="$(arch -x86_64 env -u JELLO_JIT_DISABLE "${JLO_TOOL}" run "${bin}" 2>"${TMP}/${name}.j.err" | head -1 || true)"
  if [[ "${interp}" != "${want}" ]]; then
    fail "${name} interp: expected [${want}] got [${interp}]"
  elif [[ "${jit}" != "${want}" ]]; then
    fail "${name} jit: expected [${want}] got [${jit}] (interp=${interp})"
  else
    echo "stress_reg_pressure_x64: ${name} interp=jit=${jit}"
  fi
}

run_pair "reg_pressure_gpr" "RESULT kind=I32 value=136"
run_pair "reg_pressure_xmm" "RESULT kind=F64 value=136"
run_pair "reg_pressure_mixed" "RESULT kind=I32 value=156"
run_pair "reg_pressure_survive_interaction" "RESULT kind=I32 value=55"

if [[ ${failures} -gt 0 ]]; then
  echo "chapter18_stress_reg_pressure_x64: ${failures} failure(s)" >&2
  exit 1
fi
echo "chapter18_stress_reg_pressure_x64: passed"

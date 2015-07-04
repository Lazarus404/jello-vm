#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: interp_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/interp"
BENCHDIR="${ROOT}/tests/bytecode/bench"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "interp_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== correctness fixtures: every typed numeric opcode Chapter 4 implements =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  expect_file="${FIXDIR}/${base}.expect"
  if [[ ! -f "${expect_file}" ]]; then
    fail "${base}: no .expect file"
    continue
  fi
  bin="${TMP}/${base}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"
    continue
  fi
  actual="$("${JLO_TOOL}" run "${bin}" 2>"${TMP}/${base}.run.err" || true)"
  expected="$(cat "${expect_file}")"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] $(cat "${TMP}/${base}.run.err")"
  else
    echo "interp_fixtures: ${base} -> ${actual}"
  fi
done

echo "== benchmark fixtures: fib/ackerman/tak/deep_call (Sec 2.8) =="
echo "   results section, not re-asserted as a pass/fail bound by this script"
declare -A BENCH_EXPECT=(
  [fib]="RESULT kind=I32 value=46368"
  [ackerman]="RESULT kind=I32 value=509"
  [tak]="RESULT kind=I32 value=7"
  [deep_call]="RESULT kind=I32 value=50000"
)
for name in fib ackerman tak deep_call; do
  fixture="${BENCHDIR}/${name}.jlo.txt"
  bin="${TMP}/${name}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${name}.asm.err"; then
    fail "${name}: asm failed -- $(cat "${TMP}/${name}.asm.err")"
    continue
  fi
  actual="$("${JLO_TOOL}" run "${bin}" 2>"${TMP}/${name}.run.err" || true)"
  expected="${BENCH_EXPECT[${name}]}"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${name}: expected [${expected}] got [${actual}] $(cat "${TMP}/${name}.run.err")"
  else
    echo "interp_fixtures: ${name} -> ${actual}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "chapter4_interpreter_core: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter4_interpreter_core: passed"

#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: jdll_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/jdll"
CORRUPTDIR="${FIXDIR}/corrupt"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

: "${JELLO_JDLL_PATH:?JELLO_JDLL_PATH must point at the directory containing test_native.jdll (set by tests/CMakeLists.txt)}"

failures=0
fail() {
  echo "jdll_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== round-trip fixtures: JOP_CALL_NATIVE via test_native.jdll (Chapter 7) =="
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
    echo "jdll_fixtures: ${base} -> ${actual}"
  fi
done

echo "== corrupt fixtures: declared import signature must match the real .jdll =="
for fixture in "${CORRUPTDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  bin="${TMP}/corrupt_${base}.jlo"
  err="${TMP}/corrupt_${base}.err"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${err}"; then
    fail "${base}: expected asm to succeed (fixture is structurally valid; only the .jdll resolution is wrong) -- $(cat "${err}")"
    continue
  fi
  if "${JLO_TOOL}" run "${bin}" >"${TMP}/corrupt_${base}.out" 2>"${err}"; then
    fail "${base}: expected load-time rejection, tool accepted and ran it"
  else
    echo "jdll_fixtures: ${base} correctly rejected ($(cat "${err}"))"
  fi
done

echo "== std JDLL (file I/O, get_env, argv) =="
echo "jdll_fixtures: covered by chapter7_threads_and_stop_the_world (tests/csrc/chapter7_thread_tests.c) -- see that fixture's own test_std_jdll_* functions"

if [[ ${failures} -gt 0 ]]; then
  echo "chapter7_jdll_and_std: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter7_jdll_and_std: passed"

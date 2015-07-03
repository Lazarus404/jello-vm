#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: bytecode_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode"
CORRUPTDIR="${FIXDIR}/corrupt"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "bytecode_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

roundtrip_one() {
  local fixture="$1"
  local base
  base="$(basename "${fixture}" .jlo.txt)"
  local bin="${TMP}/${base}.jlo"
  local out="${TMP}/${base}.roundtrip.txt"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}"; then
    fail "${base}: asm failed"
    return
  fi
  if ! "${JLO_TOOL}" disasm "${bin}" -o "${out}"; then
    fail "${base}: disasm failed"
    return
  fi
  if ! diff -u "${fixture}" "${out}" > "${TMP}/${base}.diff"; then
    fail "${base}: round-trip mismatch (see below)"
    cat "${TMP}/${base}.diff" >&2
  else
    echo "bytecode_fixtures: ${base} round-trips byte-identically"
  fi
}

echo "== accept fixtures: structural + semantic round-trip =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  roundtrip_one "${fixture}"
done

echo "== compressed_conformance: FULL vs COMPACT decode identically =="
full_out="${TMP}/full_disasm.txt"
compact_out="${TMP}/compact_disasm.txt"
"${JLO_TOOL}" asm "${FIXDIR}/compressed_conformance.jlo.txt" -o "${TMP}/full.jlo"
"${JLO_TOOL}" disasm "${TMP}/full.jlo" -o "${full_out}"
"${JLO_TOOL}" asm "${FIXDIR}/compressed_conformance_compact.jlo.txt" -o "${TMP}/compact.jlo"
"${JLO_TOOL}" disasm "${TMP}/compact.jlo" -o "${compact_out}"
if ! diff -u <(sed -n '/^func\[/,$p' "${full_out}") <(sed -n '/^func\[/,$p' "${compact_out}") > "${TMP}/compressed.diff"; then
  fail "compressed_conformance: FULL and COMPACT encodings decoded differently"
  cat "${TMP}/compressed.diff" >&2
else
  echo "bytecode_fixtures: compressed_conformance decodes identically FULL vs COMPACT"
fi

echo "== corrupt binary fixtures: must be rejected, never crash (structural, Chapter 2) =="
"${JLO_TOOL}" asm "${FIXDIR}/minimal_valid.jlo.txt" -o "${TMP}/valid.jlo"

valid_size=$(wc -c < "${TMP}/valid.jlo")
truncated_size=$((valid_size / 2))
head -c "${truncated_size}" "${TMP}/valid.jlo" > "${TMP}/truncated.jlo"
if "${JLO_TOOL}" disasm "${TMP}/truncated.jlo" -o "${TMP}/truncated_out.txt" 2>"${TMP}/truncated.err"; then
  fail "truncated.jlo: expected rejection, tool accepted it"
else
  echo "bytecode_fixtures: truncated.jlo correctly rejected ($(cat "${TMP}/truncated.err"))"
fi

cp "${TMP}/valid.jlo" "${TMP}/bad_magic.jlo"
printf '\x00\x00\x00\x00' | dd of="${TMP}/bad_magic.jlo" bs=1 seek=0 count=4 conv=notrunc status=none
if "${JLO_TOOL}" disasm "${TMP}/bad_magic.jlo" -o "${TMP}/bad_magic_out.txt" 2>"${TMP}/bad_magic.err"; then
  fail "bad_magic.jlo: expected rejection, tool accepted it"
else
  echo "bytecode_fixtures: bad_magic.jlo correctly rejected ($(cat "${TMP}/bad_magic.err"))"
fi

echo "== corrupt semantic fixtures: structurally valid, semantically invalid (Chapter 3) =="
for fixture in "${CORRUPTDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  bin="${TMP}/corrupt_${base}.jlo"
  out="${TMP}/corrupt_${base}_out.txt"
  err="${TMP}/corrupt_${base}.err"

  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${err}"; then
    fail "${base}: expected asm to succeed (fixture is structurally valid; only semantics are wrong) -- $(cat "${err}")"
    continue
  fi
  if "${JLO_TOOL}" disasm "${bin}" -o "${out}" 2>"${err}"; then
    fail "${base}: expected disasm to reject this module, tool accepted it"
  else
    echo "bytecode_fixtures: ${base} correctly rejected ($(cat "${err}"))"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "chapter3_bytecode_and_validation: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter3_bytecode_and_validation: passed"

| # | Chapter | Builds on | Exit gate |
|---|---|---|---|
| 1 | Repo & build skeleton | jello rules only | Empty skeleton compiles on x64 + ARM64 |
| 2 | ISA spec + fixture encoder | Ch 1 | Hand-encoded `.jlo` fixtures validate structurally |
| 3 | Loader + validation | Ch 2 | Reject/accept pass |
| 4 | Interpreter core | Ch 3 | Correctness fixtures pass; becomes the speed baseline |
| 5 | GC | Ch 4 | Allocation-churn stress fixtures pass; pause budget measured |
| 6 | Dynamic boundary + object model | Ch 4 & 5 | Object/field pass |
| 7 | Threads + JDLL spine + std JDLL v1 | Ch 5 & 6 | Plugin round-trip + blocking + file I/O fixtures pass |
| 8 | Bytes-layout + packet ops | Ch 6 & 7 | Pack/unpack round-trip, zero Dynamic on hot path |
| 9 | Shared IR + lowering tables | Ch 4 | Interpreter re-derived from tables; Ch 3-8 fixtures re-pass bit-for-bit |
| 10 | JIT x64: IR + frame/call ABI | Ch 9 | Frame layout + private call ABI documented and round-trips a no-op function |
| 11 | JIT x64: numeric op emit | Ch 10 | Per-opcode conformance fixtures for typed arithmetic/compare/move |
| 12 | JIT x64: control flow emit | Ch 11 | Branch/loop conformance fixtures; OSR/loop-anchor entry points identified |
| 13 | JIT x64: calls + module blob (W^X) | Ch 12 | Direct JIT-to-JIT calls, no C-helper round trip; beats Ch 4 baseline |
| 14 | JIT x64: object/Dynamic/bytes op emit | Ch 6, 8 & 13 | Object, Dynamic-boundary, and bytes-layout fixtures pass under JIT |
| 15 | JIT x64 register-safe calls | Ch 13 | Armed/take counters + nested-call fixtures pass |
| 16 | Hot reload (x64 only) | Ch 13, Ch 15 | Warm-compile -> patch -> re-run, including signature-conflict and type-change detection |
| 17 | JIT ARM64 mirror | Ch 10-16 | Same fixture suite passes on both arches from the same tables |
| 18 | Fuel/limits + traps | Ch 10-17 | Sandboxed fixtures pass; native PC -> bytecode PC resolves |
| 19 | Stress-testing the VM (nine lessons) | ALL | Every stress fixture passes under a stated resource ceiling; register-allocator overflow, N-way thread contention, and a combined capstone all proven correct |
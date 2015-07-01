// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_VM_INTERNAL_H
#define JELLO_VM_INTERNAL_H

#include <jello.h>
#include <jello/internal/bytecode.h>

/* Internal VM layouts, frames, dispatch, GC fields. */

/* -- the concrete module (Chapter 3) -- */
struct jello_bc_module {
  uint32_t version;
  uint32_t features;
  uint32_t ntypes;
  const jello_type_entry* types;
  uint32_t nsigs;
  const jello_fun_sig* sigs;
  uint32_t nstruct_sigs;
  const jello_struct_sig* struct_sigs;
  uint32_t natoms;
  const char* atoms_data;
  const char* const* atoms;
  uint32_t nconst_i64;
  const int64_t* const_i64;
  uint32_t nconst_f64;
  const double* const_f64;
  uint32_t nconst_bytes;
  const uint32_t* const_bytes_len;
  const uint32_t* const_bytes_off;
  const uint8_t* const_bytes_data;
  uint32_t nfuncs;
  const jello_bc_function* funcs;
  uint32_t entry;
};

/* -- semantic validation (check.c) -- */
jello_bc_result jello_bc_validate_insn(
    const jello_bc_module* m,
    const jello_type_id* reg_types,
    const jello_insn* ins,
    uint32_t nregs,
    uint32_t pc,
    uint32_t ninsns,
    uint32_t nfuncs);

#endif /* JELLO_VM_INTERNAL_H */

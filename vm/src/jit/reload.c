// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

/* -- Signature and body-hash comparison -- */
static uint64_t fnv1a_update(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for(size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t hash_type(const jello_bc_module* m, jello_type_id tid, uint64_t h, int depth) {
  if(tid >= m->ntypes || depth > 8) return fnv1a_update(h, &tid, sizeof(tid));
  jello_type_kind k = m->types[tid].kind;
  h = fnv1a_update(h, &k, sizeof(k));
  if(k != JELLO_T_STRUCT) return h;
  uint32_t sig_id = m->types[tid].as.strct.struct_sig_id;
  if(sig_id >= m->nstruct_sigs) return h;
  const jello_struct_sig* sig = &m->struct_sigs[sig_id];
  h = fnv1a_update(h, &sig->nfields, sizeof(sig->nfields));
  h = fnv1a_update(h, &sig->is_mutable, sizeof(sig->is_mutable));
  for(uint32_t i = 0; i < sig->nfields; i++) {
    h = hash_type(m, sig->field_type[i], h, depth + 1);
    if(sig->named && sig->field_name_atom) {
      uint32_t atom = sig->field_name_atom[i];
      if(atom < m->natoms) h = fnv1a_update(h, m->atoms[atom], strlen(m->atoms[atom]));
    }
  }
  return h;
}

typedef struct jello_reload_signature {
  uint64_t ret_hash;
  uint32_t nargs;
  uint64_t arg_hash[256]; /* check.c's own frozen 256-register ceiling */
} jello_reload_signature;

static int compute_signature(const jello_bc_module* m, const jello_bc_function* f,
    jello_reload_signature* out) {
  out->ret_hash = 0;
  int saw_ret = 0;
  for(uint32_t pc = 0; pc < f->ninsns; pc++) {
    if((jello_op)f->insns[pc].op != JOP_RET) continue;
    uint32_t reg = f->insns[pc].a;
    if(reg >= f->nregs) return 0;
    uint64_t rh = hash_type(m, f->reg_types[reg], 1469598103934665603ULL, 0);
    if(saw_ret && rh != out->ret_hash) return 0;
    out->ret_hash = rh;
    saw_ret = 1;
  }
  out->nargs = f->nargs;
  for(uint32_t i = 0; i < f->nargs; i++) {
    out->arg_hash[i] = hash_type(m, f->reg_types[i], 1469598103934665603ULL, 0);
  }
  return 1;
}

static int signatures_match(const jello_reload_signature* a, const jello_reload_signature* b) {
  if(a->ret_hash != b->ret_hash) return 0;
  if(a->nargs != b->nargs) return 0;
  for(uint32_t i = 0; i < a->nargs; i++) {
    if(a->arg_hash[i] != b->arg_hash[i]) return 0;
  }
  return 1;
}

static uint64_t compute_body_hash(const jello_bc_module* m, const jello_bc_function* f) {
  uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
  for(uint32_t r = 0; r < f->nregs; r++) h = hash_type(m, f->reg_types[r], h, 0);
  if(f->ninsns > 0) h = fnv1a_update(h, f->insns, (size_t)f->ninsns * sizeof(jello_insn));
  return h;
}

jello_reload_fn_status jello_reload_analyze_function(const jello_bc_module* old_m, const jello_bc_function* old_f,
    const jello_bc_module* new_m, const jello_bc_function* new_f) {
  jello_reload_signature sig_old, sig_new;
  if(!compute_signature(old_m, old_f, &sig_old)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;
  if(!compute_signature(new_m, new_f, &sig_new)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;
  if(!signatures_match(&sig_old, &sig_new)) return JELLO_RELOAD_FN_SIGNATURE_CONFLICT;

  uint64_t hash_old = compute_body_hash(old_m, old_f);
  uint64_t hash_new = compute_body_hash(new_m, new_f);
  return (hash_old == hash_new) ? JELLO_RELOAD_FN_UNCHANGED : JELLO_RELOAD_FN_CHANGED;
}

/* -- Struct type-shape conflicts, matched positionally -- */
int jello_reload_struct_sig_changed(const jello_bc_module* old_m, const jello_bc_module* new_m, uint32_t struct_sig_id) {
  if(struct_sig_id >= old_m->nstruct_sigs || struct_sig_id >= new_m->nstruct_sigs) return 1;
  const jello_struct_sig* a = &old_m->struct_sigs[struct_sig_id];
  const jello_struct_sig* b = &new_m->struct_sigs[struct_sig_id];
  if(a->nfields != b->nfields || a->named != b->named || a->is_mutable != b->is_mutable) return 1;
  for(uint32_t i = 0; i < a->nfields; i++) {
    uint64_t ha = hash_type(old_m, a->field_type[i], 1469598103934665603ULL, 0);
    uint64_t hb = hash_type(new_m, b->field_type[i], 1469598103934665603ULL, 0);
    if(ha != hb) return 1;
    if(a->named) {
      uint32_t aa = a->field_name_atom[i], ab = b->field_name_atom[i];
      const char* na = aa < old_m->natoms ? old_m->atoms[aa] : "";
      const char* nb = ab < new_m->natoms ? new_m->atoms[ab] : "";
      if(strcmp(na, nb) != 0) return 1;
    }
  }
  return 0;
}

int jello_reload_function_uses_struct_sig(const jello_bc_module* m, const jello_bc_function* f, uint32_t struct_sig_id) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_id tid = f->reg_types[r];
    if(tid >= m->ntypes) continue;
    if(m->types[tid].kind != JELLO_T_STRUCT) continue;
    if(m->types[tid].as.strct.struct_sig_id == struct_sig_id) return 1;
  }
  return 0;
}

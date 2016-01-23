// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <stdlib.h>
#include <string.h>

static size_t module_emit_cap(const jello_bc_module* m) {
  size_t cap = 4096u;
  for(uint32_t fi = 0; fi < m->nfuncs; fi++) {
    jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
    if(!ir) continue;
    cap += (size_t)ir->ninsns * JIT_EMIT_CALL_INSN_BUDGET + 512u;
    jello_jit_ir_free(ir);
  }
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

uint32_t jello_jit_module_prepare_count;

int jello_jit_module_prepare(const jello_bc_module* m, jello_jit_module* out) {
  jello_jit_module_prepare_count++;
  memset(out, 0, sizeof(*out));
  if(!m || m->nfuncs == 0) return 0;

  out->nfuncs = m->nfuncs;
  out->funcs = (jello_jit_func_code*)calloc(m->nfuncs, sizeof(jello_jit_func_code));
  if(!out->funcs) return 0;

  out->blob = jit_alloc_exec_buf(module_emit_cap(m));
  if(!out->blob.mem) {
    free(out->funcs);
    out->funcs = NULL;
    return 0;
  }

  jello_jit_call_patch* patches = NULL;
  uint32_t npatch = 0;
  uint32_t patch_cap = 0;

  for(uint32_t fi = 0; fi < m->nfuncs; fi++) {
    if(!jello_jit_emit_function_into(m, fi, &out->blob, &out->funcs[fi],
            &patches, &npatch, &patch_cap, 1)) {
      jello_jit_module_free(out);
      free(patches);
      return 0;
    }
  }

  for(uint32_t pi = 0; pi < npatch; pi++) {
    jello_jit_call_patch* p = &patches[pi];
    if(p->callee_fi >= m->nfuncs) {
      jello_jit_module_free(out);
      free(patches);
      return 0;
    }
    size_t callee = out->funcs[p->callee_fi].blob_off + out->funcs[p->callee_fi].body_off;
    int32_t rel = (int32_t)(callee - (p->site_off + 5));
    jello_jit_patch_call_rel32(&out->blob, p->site_off, rel);
  }

  if(!jit_finalize_exec_buf(&out->blob)) {
    jello_jit_module_free(out);
    free(patches);
    return 0;
  }

  out->patches = patches;
  out->npatch = npatch;
  out->prepared = 1;
  return 1;
}

void jello_jit_module_free(jello_jit_module* jm) {
  if(!jm) return;
  if(jm->funcs) {
    for(uint32_t i = 0; i < jm->nfuncs; i++) {
      free(jm->funcs[i].loop_anchor_pcs);
      free(jm->funcs[i].loop_anchor_offs);
    }
    free(jm->funcs);
  }
  jm->funcs = NULL;
  jm->nfuncs = 0;
  jm->prepared = 0;
  free(jm->patches);
  jm->patches = NULL;
  jm->npatch = 0;
  jit_free_exec_buf(&jm->blob);
}

jello_jit_module* jello_jit_module_create(const jello_bc_module* m) {
  jello_jit_module* jm = (jello_jit_module*)calloc(1, sizeof(jello_jit_module));
  if(!jm) return NULL;
  if(!jello_jit_module_prepare(m, jm)) {
    free(jm);
    return NULL;
  }
  return jm;
}

void jello_jit_module_destroy(jello_jit_module* jm) {
  if(!jm) return;
  jello_jit_module_free(jm);
  free(jm);
}

uint32_t jit_try_enter_count;

int jello_jit_try_enter(jello_vm* vm, jello_jit_module* jm, jello_numeric_result* out,
                        jello_exec_status* out_st) {
  if(!jm || !jm->prepared || !vm || !out || !out_st) return 0;
  if(vm->call_frames_len == 0) return 0;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(fr->pc != 0 || fr->has_caller != 0) return 0;
  uint32_t fi = fr->func_index;
  if(fi >= jm->nfuncs || jm->funcs[fi].size == 0) return 0;

  jello_jit_entry_fn entry =
      (jello_jit_entry_fn)(void*)(jm->blob.mem + jm->funcs[fi].blob_off + jm->funcs[fi].entry_off);
  jit_try_enter_count++;
  *out_st = entry(vm, fr->rf.mem, out);
  return 1;
}
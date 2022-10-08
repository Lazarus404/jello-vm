// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * How a program's result and traps look to the host that ran it; jello, jelloc and
 * `jlo_tool run --print`. */

#include <jello/internal.h>
#include "host.h"

#include <stdio.h>
#include <stdlib.h>

static void print_heap(const void* p, FILE* out) {
  switch((jello_obj_kind)((const jello_obj_header*)p)->kind) {
    case JELLO_OBJ_BOX_I64:
      fprintf(out, "%lld\n", (long long)((const jello_box_i64*)p)->value);
      break;
    case JELLO_OBJ_BOX_F64: fprintf(out, "%g\n", ((const jello_box_f64*)p)->value); break;
    case JELLO_OBJ_BOX_F32: fprintf(out, "%g\n", (double)((const jello_box_f32*)p)->value); break;
    case JELLO_OBJ_BOX_F16:
      fprintf(out, "%g\n", (double)vm_f16_to_f32(((const jello_box_f16*)p)->value));
      break;
    case JELLO_OBJ_STRUCT: fputs("<tuple or record>\n", out); break;
    case JELLO_OBJ_OBJECT: fputs("<object>\n", out); break;
    case JELLO_OBJ_ARRAY: fputs("<array>\n", out); break;
    case JELLO_OBJ_FUNCTION: fputs("<function>\n", out); break;
    case JELLO_OBJ_LIST: fputs("<list>\n", out); break;
    case JELLO_OBJ_ENUM: fputs("<enum value>\n", out); break;
    case JELLO_OBJ_HANDLE: fputs("<native handle>\n", out); break;
    default: fputs("<value>\n", out); break;
  }
}

void jello_print_host_result(const jello_bc_module* m, const jello_numeric_result* r, FILE* out,
                             int bytes_newline) {
  const jello_bytes* b = vm_result_bytes(r);
  if(b) {
    fwrite(b->data, 1, b->length, out);
    if(bytes_newline) fputc('\n', out);
    return;
  }
  switch(r->kind) {
    case JELLO_NUM_VOID: break;
    case JELLO_NUM_BOOL: fputs(r->as.b ? "true\n" : "false\n", out); break;
    case JELLO_NUM_I32: fprintf(out, "%d\n", r->as.i32); break;
    case JELLO_NUM_I64: fprintf(out, "%lld\n", (long long)r->as.i64); break;
    case JELLO_NUM_F32: fprintf(out, "%g\n", (double)r->as.f32); break;
    case JELLO_NUM_F64: fprintf(out, "%g\n", r->as.f64); break;
    case JELLO_NUM_PTR:
      if(r->as.ptr) print_heap(r->as.ptr, out);
      break;
    case JELLO_NUM_DYN: {
      jello_value v = r->as.dyn;
      if(jello_is_null(v)) break;
      if(jello_is_bool(v)) fputs(jello_as_bool(v) ? "true\n" : "false\n", out);
      else if(jello_is_i32(v)) fprintf(out, "%d\n", jello_as_i32(v));
      else if(jello_is_atom(v) && jello_as_atom(v) < m->natoms)
        fprintf(out, "%s\n", m->atoms[jello_as_atom(v)]);
      else if(jello_is_ptr(v) && jello_as_ptr(v)) print_heap(jello_as_ptr(v), out);
      else fputs("<value>\n", out);
      break;
    }
  }
}

/*
 * Copyright 2026 SotC-Android contributors
 * SPDX-License-Identifier: MIT
 *
 * VK_KHR_fragment_shader_barycentric for ir3 (local patch for the SotC-Android port, not
 * submitted upstream).
 *
 * The hardware interpolates a varying with bary.f, which is affine in the two coordinates (i, j)
 * the rasterizer gives the shader: value(i, j) = A + B*i + C*j. So
 *  - the barycentric coordinates are (1 - i - j, i, j), as the Intel driver lowers them;
 *  - the values at the three vertices are value(0,0), value(1,0) and value(0,1): bary.f with
 *    constant coordinates, as ir3 already interpolates at arbitrary (i, j) for
 *    interpolateAtOffset.
 * With both taken from the same (i, j) parameterization, sum(bary[k] * vertex[k]) equals the
 * hardware's own interpolation, which is how shaders use them (the PS4 games that interpolate
 * by hand, Shadow of the Colossus among them).
 *
 * Integer per-vertex inputs cannot go through bary.f: they read the provoking vertex's value.
 */

#include "compiler/nir/nir_builder.h"
#include "ir3_nir.h"

#include <string.h>

static nir_intrinsic_op
coord_to_ij_op(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_load_barycentric_coord_pixel:
      return nir_intrinsic_load_barycentric_pixel;
   case nir_intrinsic_load_barycentric_coord_centroid:
      return nir_intrinsic_load_barycentric_centroid;
   case nir_intrinsic_load_barycentric_coord_sample:
      return nir_intrinsic_load_barycentric_sample;
   case nir_intrinsic_load_barycentric_coord_at_sample:
      return nir_intrinsic_load_barycentric_at_sample;
   case nir_intrinsic_load_barycentric_coord_at_offset:
      return nir_intrinsic_load_barycentric_at_offset;
   default:
      return nir_num_intrinsics;
   }
}

static nir_def *
lower_coord(nir_builder *b, nir_intrinsic_instr *intr, nir_intrinsic_op ij_op)
{
   const unsigned mode = nir_intrinsic_interp_mode(intr);
   nir_def *ij;
   switch (ij_op) {
   case nir_intrinsic_load_barycentric_at_sample:
      ij = nir_load_barycentric_at_sample(b, 32, intr->src[0].ssa, .interp_mode = mode);
      break;
   case nir_intrinsic_load_barycentric_at_offset:
      ij = nir_load_barycentric_at_offset(b, 32, intr->src[0].ssa, .interp_mode = mode);
      break;
   default:
      ij = nir_load_barycentric(b, ij_op, mode);
      break;
   }
   nir_def *i = nir_channel(b, ij, 0);
   nir_def *j = nir_channel(b, ij, 1);
   return nir_vec3(b, nir_fsub_imm(b, 1.0f, nir_fadd(b, i, j)), i, j);
}

static nir_def *
load_corner(nir_builder *b, nir_intrinsic_instr *intr, float i, float j)
{
   return nir_load_interpolated_input(
      b, intr->def.num_components, intr->def.bit_size, nir_imm_vec2(b, i, j),
      intr->src[1].ssa, .base = nir_intrinsic_base(intr),
      .component = nir_intrinsic_component(intr),
      .dest_type = nir_intrinsic_dest_type(intr),
      .io_semantics = nir_intrinsic_io_semantics(intr));
}

static nir_def *
lower_input_vertex(nir_builder *b, nir_intrinsic_instr *intr)
{
   const nir_alu_type type = nir_intrinsic_dest_type(intr);
   if (nir_alu_type_get_base_type(type) != nir_type_float) {
      return nir_load_input(b, intr->def.num_components, intr->def.bit_size,
                            intr->src[1].ssa, .base = nir_intrinsic_base(intr),
                            .component = nir_intrinsic_component(intr), .dest_type = type,
                            .io_semantics = nir_intrinsic_io_semantics(intr));
   }

   nir_def *vertex = intr->src[0].ssa;
   if (nir_src_is_const(intr->src[0])) {
      switch (nir_src_as_uint(intr->src[0])) {
      case 0:
         return load_corner(b, intr, 0.0f, 0.0f);
      case 1:
         return load_corner(b, intr, 1.0f, 0.0f);
      default:
         return load_corner(b, intr, 0.0f, 1.0f);
      }
   }
   nir_def *v0 = load_corner(b, intr, 0.0f, 0.0f);
   nir_def *v1 = load_corner(b, intr, 1.0f, 0.0f);
   nir_def *v2 = load_corner(b, intr, 0.0f, 1.0f);
   return nir_bcsel(b, nir_ieq_imm(b, vertex, 0), v0,
                    nir_bcsel(b, nir_ieq_imm(b, vertex, 1), v1, v2));
}

static bool
lower_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const nir_intrinsic_op ij_op = coord_to_ij_op(intr->intrinsic);
   if (ij_op == nir_num_intrinsics && intr->intrinsic != nir_intrinsic_load_input_vertex)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *res = ij_op != nir_num_intrinsics ? lower_coord(b, intr, ij_op)
                                              : lower_input_vertex(b, intr);
   nir_def_replace(&intr->def, res);
   return true;
}

bool
ir3_nir_lower_fs_barycentric(nir_shader *shader)
{
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);
   return nir_shader_intrinsics_pass(shader, lower_instr, nir_metadata_control_flow, NULL);
}

/*
 * Exact per-vertex bits. bary.f is float arithmetic: at fixed coordinates it gives a vertex's
 * value only approximately (low mantissa bits rounded, denormals flushed, NaN payloads lost).
 * Games read per-vertex inputs as raw bits: Shadow of the Colossus packs two halves in each
 * 32-bit value, and the low half sits in the low mantissa bits (smeared grass, faceted rocks).
 *
 * So at link time, for every 32-bit component the fragment shader reads only per vertex, the
 * stage before it writes the two 16-bit halves as floats (0..65535: integers that small survive
 * any interpolation arithmetic exactly), the low one in the original component and the high one
 * in a free generic slot, and the fragment shader puts the bits back together.
 */
#define PV_SLOTS 32 /* VARYING_SLOT_VAR0..VAR31 */

struct pv_state {
   uint8_t per_vertex[PV_SLOTS]; /* components read with load_input_vertex */
   uint8_t other[PV_SLOTS];      /* components read any other way */
   uint8_t encoded[PV_SLOTS];
   uint8_t hi_slot[PV_SLOTS][4]; /* where the high half of each encoded component goes */
   uint8_t hi_comp[PV_SLOTS][4];
};

static bool
io_slot(nir_intrinsic_instr *intr, nir_src *offset, unsigned *slot)
{
   if (!nir_src_is_const(*offset))
      return false;
   const unsigned s = nir_intrinsic_io_semantics(intr).location + nir_src_as_uint(*offset);
   if (s < VARYING_SLOT_VAR0 || s - VARYING_SLOT_VAR0 >= PV_SLOTS)
      return false;
   *slot = s - VARYING_SLOT_VAR0;
   return true;
}

static bool
gather_fs_inputs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct pv_state *st = data;
   nir_src *offset;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input_vertex:
   case nir_intrinsic_load_interpolated_input:
      offset = &intr->src[1];
      break;
   case nir_intrinsic_load_input:
      offset = &intr->src[0];
      break;
   default:
      return false;
   }
   const uint8_t comps =
      BITFIELD_MASK(intr->def.num_components) << nir_intrinsic_component(intr);
   unsigned slot;
   if (!io_slot(intr, offset, &slot)) {
      /* indirect: nothing it might touch gets encoded */
      for (unsigned s = 0; s < PV_SLOTS; s++)
         st->other[s] = 0xf;
      return false;
   }
   if (intr->intrinsic == nir_intrinsic_load_input_vertex && intr->def.bit_size == 32)
      st->per_vertex[slot] |= comps;
   else
      st->other[slot] |= comps;
   return false;
}

static nir_io_semantics
hi_semantics(nir_intrinsic_instr *intr, unsigned hi_slot)
{
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   sem.location = VARYING_SLOT_VAR0 + hi_slot;
   sem.num_slots = 1;
   return sem;
}

static bool
decode_fs_input(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct pv_state *st = data;
   unsigned slot;
   if (intr->intrinsic != nir_intrinsic_load_input_vertex || intr->def.bit_size != 32 ||
       !io_slot(intr, &intr->src[1], &slot))
      return false;
   const unsigned first = nir_intrinsic_component(intr);
   const uint8_t mask = (st->encoded[slot] >> first) & BITFIELD_MASK(intr->def.num_components);
   if (!mask)
      return false;
   nir_intrinsic_set_dest_type(intr, nir_type_float32);
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      nir_def *v = nir_channel(b, &intr->def, c);
      if (!(mask & (1u << c))) {
         comps[c] = v;
         continue;
      }
      const unsigned oc = first + c;
      nir_def *hi = nir_load_input_vertex(
         b, 1, 32, intr->src[0].ssa, nir_imm_int(b, 0), .base = 0,
         .component = st->hi_comp[slot][oc], .dest_type = nir_type_float32,
         .io_semantics = hi_semantics(intr, st->hi_slot[slot][oc]));
      nir_def *lo_bits = nir_f2u32(b, nir_fround_even(b, v));
      nir_def *hi_bits = nir_f2u32(b, nir_fround_even(b, hi));
      comps[c] = nir_ior(b, lo_bits, nir_ishl_imm(b, hi_bits, 16));
   }
   nir_def *decoded = nir_vec(b, comps, intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, decoded);
   return true;
}

static bool
encode_output(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct pv_state *st = data;
   unsigned slot;
   if (intr->intrinsic != nir_intrinsic_store_output || nir_src_bit_size(intr->src[0]) != 32 ||
       !io_slot(intr, &intr->src[1], &slot))
      return false;
   const unsigned first = nir_intrinsic_component(intr);
   const unsigned wrmask = nir_intrinsic_write_mask(intr);
   const uint8_t mask = (st->encoded[slot] >> first) & wrmask;
   if (!mask)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *value = intr->src[0].ssa;
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < value->num_components; c++) {
      nir_def *v = nir_channel(b, value, c);
      if (!(mask & (1u << c))) {
         comps[c] = v;
         continue;
      }
      const unsigned oc = first + c;
      comps[c] = nir_u2f32(b, nir_iand_imm(b, v, 0xffff));
      nir_store_output(b, nir_u2f32(b, nir_ushr_imm(b, v, 16)), nir_imm_int(b, 0), .base = 0,
                       .component = st->hi_comp[slot][oc], .write_mask = 1,
                       .src_type = nir_type_float32,
                       .io_semantics = hi_semantics(intr, st->hi_slot[slot][oc]));
   }
   nir_intrinsic_set_src_type(intr, nir_type_float32);
   nir_src_rewrite(&intr->src[0], nir_vec(b, comps, value->num_components));
   return true;
}

void
ir3_nir_encode_per_vertex_inputs(nir_shader *producer, nir_shader *fs)
{
   if (!producer || !fs || fs->info.stage != MESA_SHADER_FRAGMENT)
      return;
   struct pv_state st;
   memset(&st, 0, sizeof(st));
   nir_shader_intrinsics_pass(fs, gather_fs_inputs, nir_metadata_all, &st);

   /* free generic slots for the high halves */
   const uint64_t used = producer->info.outputs_written | fs->info.inputs_read;
   unsigned free_slots[PV_SLOTS], num_free = 0;
   for (unsigned s = 0; s < PV_SLOTS; s++) {
      if (!(used & BITFIELD64_BIT(VARYING_SLOT_VAR0 + s)))
         free_slots[num_free++] = s;
   }
   unsigned next = 0; /* high halves placed so far, four to a slot */
   for (unsigned s = 0; s < PV_SLOTS; s++) {
      const uint8_t mask = st.per_vertex[s] & ~st.other[s];
      for (unsigned c = 0; c < 4; c++) {
         if (!(mask & (1u << c)) || next / 4 >= num_free)
            continue;
         st.encoded[s] |= 1u << c;
         st.hi_slot[s][c] = free_slots[next / 4];
         st.hi_comp[s][c] = next % 4;
         ++next;
      }
   }
   if (!next)
      return;

   nir_shader_intrinsics_pass(producer, encode_output, nir_metadata_control_flow, &st);
   nir_shader_intrinsics_pass(fs, decode_fs_input, nir_metadata_control_flow, &st);
   for (unsigned k = 0; k < (next + 3) / 4; k++) {
      producer->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + free_slots[k]);
      fs->info.inputs_read |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + free_slots[k]);
   }
   nir_recompute_io_bases(producer, nir_var_shader_out);
   nir_recompute_io_bases(fs, nir_var_shader_in);
}

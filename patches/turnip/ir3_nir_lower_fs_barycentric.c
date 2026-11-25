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

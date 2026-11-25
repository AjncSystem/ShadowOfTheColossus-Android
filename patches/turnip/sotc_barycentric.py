#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""SotC-Android local Turnip patch: VK_KHR_fragment_shader_barycentric on ir3.

Run from the Mesa source root, after StevenMXZ/Adreno-Tools-Drivers' own patches. Every edit is
anchored on exact source text and fails if Mesa changed there. Not submitted upstream.
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def edit(path: str, old: str, new: str, what: str) -> None:
    p = Path(path)
    text = p.read_text()
    if new in text:
        return  # already applied
    count = text.count(old)
    if count != 1:
        sys.exit(f"sotc_barycentric: {what}: anchor found {count} times in {path}")
    p.write_text(text.replace(old, new, 1))


# The extension, its feature and its one property.
edit("src/freedreno/vulkan/tu_device.cc",
     "      .KHR_fragment_shading_rate = device->info->props.has_attachment_shading_rate,\n",
     "      .KHR_fragment_shader_barycentric = true,\n"
     "      .KHR_fragment_shading_rate = device->info->props.has_attachment_shading_rate,\n",
     "extension")
edit("src/freedreno/vulkan/tu_device.cc",
     "   features->shaderSubgroupRotateClustered = true;\n",
     "   features->shaderSubgroupRotateClustered = true;\n\n"
     "   /* VK_KHR_fragment_shader_barycentric (SotC-Android local patch) */\n"
     "   features->fragmentShaderBarycentric = true;\n",
     "feature")
edit("src/freedreno/vulkan/tu_device.cc",
     "   props->transformFeedbackPreservesTriangleFanProvokingVertex = false;\n",
     "   props->transformFeedbackPreservesTriangleFanProvokingVertex = false;\n\n"
     "   /* VK_KHR_fragment_shader_barycentric (SotC-Android local patch) */\n"
     "   props->triStripVertexOrderIndependentOfProvokingVertex = false;\n",
     "property")

# The lowering pass, registered and run first among the fragment shader passes.
shutil.copy2(HERE / "ir3_nir_lower_fs_barycentric.c",
             "src/freedreno/ir3/ir3_nir_lower_fs_barycentric.c")
edit("src/freedreno/ir3/meson.build",
     "  'ir3_nir_lower_load_barycentric_at_offset.c',\n",
     "  'ir3_nir_lower_load_barycentric_at_offset.c',\n"
     "  'ir3_nir_lower_fs_barycentric.c',\n",
     "meson source")
edit("src/freedreno/ir3/ir3_nir.h",
     "bool ir3_nir_lower_load_barycentric_at_offset(nir_shader *shader);\n",
     "bool ir3_nir_lower_load_barycentric_at_offset(nir_shader *shader);\n"
     "bool ir3_nir_lower_fs_barycentric(nir_shader *shader);\n"
     "void ir3_nir_encode_per_vertex_inputs(nir_shader *producer, nir_shader *fs);\n",
     "declaration")

# Exact per-vertex bits: encoded by the stage before the fragment shader at link time.
edit("src/freedreno/vulkan/tu_shader.cc",
     "   nir_opt_varyings_bulk(link_shaders, link_shaders_count, true, UINT32_MAX,\n"
     "                         UINT32_MAX, link_opts, dev->compiler, NULL);\n",
     "   nir_opt_varyings_bulk(link_shaders, link_shaders_count, true, UINT32_MAX,\n"
     "                         UINT32_MAX, link_opts, dev->compiler, NULL);\n\n"
     "   /* VK_KHR_fragment_shader_barycentric (SotC-Android local patch) */\n"
     "   if (link_shaders_count >= 2 &&\n"
     "       link_shaders[link_shaders_count - 1]->info.stage == MESA_SHADER_FRAGMENT)\n"
     "      ir3_nir_encode_per_vertex_inputs(link_shaders[link_shaders_count - 2],\n"
     "                                       link_shaders[link_shaders_count - 1]);\n",
     "link encode")
edit("src/freedreno/ir3/ir3_nir.c",
     "      NIR_PASS(_, s, nir_opt_barycentric, true);\n",
     "      NIR_PASS(_, s, ir3_nir_lower_fs_barycentric);\n"
     "      NIR_PASS(_, s, nir_opt_barycentric, true);\n",
     "pass call")

# Colour inputs check the interpolation mode of the instruction that made (i, j); the per-vertex
# loads use constants there.
edit("src/freedreno/ir3/ir3_compiler_nir.c",
     "      nir_intrinsic_instr *baryc = nir_def_as_intrinsic(intr->src[0].ssa);\n"
     "      if (nir_intrinsic_interp_mode(baryc) == INTERP_MODE_NONE)\n",
     "      nir_intrinsic_instr *baryc = nir_src_as_intrinsic(intr->src[0]);\n"
     "      if (baryc && nir_intrinsic_has_interp_mode(baryc) &&\n"
     "          nir_intrinsic_interp_mode(baryc) == INTERP_MODE_NONE)\n",
     "colour input guard")

print("sotc_barycentric: VK_KHR_fragment_shader_barycentric applied")

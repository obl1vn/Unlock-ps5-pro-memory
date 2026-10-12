#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Add PS5 tuning switches to ggml-vulkan.cpp (llama.cpp's Vulkan backend).

Every switch is an environment variable. When none is set, the backend behaves
exactly as before. The bench app sets them from env.txt in its folder.

  GGML_VK_PS5_RM_STDQ=<n>      rows per workgroup factor for Q4_0/Q4_1/Q5/Q8_0 mat-vec
  GGML_VK_PS5_RM_KQ=<n>        same for the K-quants (Q2_K..Q6_K)
  GGML_VK_PS5_DMMV_WG=0|1      mat-vec workgroup: 0 = one subgroup, 1 = four subgroups
  GGML_VK_PS5_DMMV_SUBGROUP=n  subgroup (wave) size for mat-vec shaders, e.g. 32 or 64
  GGML_VK_PS5_SUBMIT_MFLOP=n   submit GPU work every n million FLOPs (0 = only by node count)

The backend also logs the GPU architecture it detected and the values in use.

Usage: python3 ggml-vulkan-ps5-knobs.py <llama.cpp source dir>
The edits are guarded: running it twice is harmless, and it stops if the
source no longer matches what it expects.
"""
from pathlib import Path
import sys

MARK = "/* ps5-knobs */"


def main():
    path = Path(sys.argv[1]) / "ggml/src/ggml-vulkan/ggml-vulkan.cpp"
    text = path.read_text()
    if MARK in text:
        print(f"{path}: PS5 knobs already applied")
        return

    def replace(before, after, count=1):
        nonlocal text
        found = text.count(before)
        if found != count:
            raise SystemExit(f"{path}: expected {count} match(es), found {found}:\n{before}")
        text = text.replace(before, after)

    # 1. Helpers, placed before the first function that uses them.
    replace("static void ggml_vk_load_shaders(vk_device& device, vk_pipeline requested) {",
            MARK + """
static uint32_t ggml_vk_ps5_env_u32(const char * name, uint32_t fallback) {
    const char * value = getenv(name);
    if (value == nullptr || *value == 0) {
        return fallback;
    }
    char * end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    return (end != value && parsed > 0 && parsed <= 1024) ? (uint32_t) parsed : fallback;
}

static uint32_t ggml_vk_ps5_dmmv_wg(uint32_t chosen) {
    const char * value = getenv("GGML_VK_PS5_DMMV_WG");
    if (value != nullptr && (value[0] == '0' || value[0] == '1') && value[1] == 0) {
        return value[0] == '0' ? DMMV_WG_SIZE_SUBGROUP : DMMV_WG_SIZE_LARGE;
    }
    return chosen;
}

static void ggml_vk_load_shaders(vk_device& device, vk_pipeline requested) {""")

    # 2. Rows per workgroup, overridable; log what is used.
    replace("    uint32_t rm_iq = 2 * rm_kq;\n",
            """    rm_stdq = ggml_vk_ps5_env_u32("GGML_VK_PS5_RM_STDQ", rm_stdq);
    rm_kq = ggml_vk_ps5_env_u32("GGML_VK_PS5_RM_KQ", rm_kq);
    uint32_t rm_iq = 2 * rm_kq;
""")

    # 3. Subgroup (wave) size used by the mat-vec shaders, overridable within
    #    the range the device allows; log the device's numbers either way.
    before = ("    const uint32_t subgroup_size = (device->vendor_id == VK_VENDOR_ID_INTEL && "
              "device->subgroup_size_control && device->subgroup_min_size <= 16 && "
              "device->subgroup_max_size >= 16) ? 16 : device->subgroup_size;\n")
    replace(before, before.replace("const uint32_t subgroup_size =", "uint32_t subgroup_size =") + """    {
        const uint32_t wanted = ggml_vk_ps5_env_u32("GGML_VK_PS5_DMMV_SUBGROUP", 0);
        if (wanted != 0 && device->subgroup_size_control &&
            wanted >= device->subgroup_min_size && wanted <= device->subgroup_max_size) {
            subgroup_size = wanted;
        }
        static bool ps5_knobs_logged = false;
        if (!ps5_knobs_logged) {
        ps5_knobs_logged = true;
        GGML_LOG_INFO("ggml_vulkan: ps5-knobs arch=%d subgroup=%u (device %u, min %u, max %u, size_control %d, "
                      "subgroup_arithmetic %d) rm_stdq=%u rm_kq=%u dmmv_wg_env=%s\\n",
                      (int) device->architecture, subgroup_size, device->subgroup_size, device->subgroup_min_size,
                      device->subgroup_max_size, (int) device->subgroup_size_control,
                      (int) device->subgroup_arithmetic, rm_stdq, rm_kq,
                      getenv("GGML_VK_PS5_DMMV_WG") ? getenv("GGML_VK_PS5_DMMV_WG") : "unset");
        }
    }
""")

    # 4. Workgroup size choice at dispatch time, overridable.
    replace("pipeline_dequant_mul_mat_vec_q8_1_f32[dmmv_wg]",
            "pipeline_dequant_mul_mat_vec_q8_1_f32[ggml_vk_ps5_dmmv_wg(dmmv_wg)]")
    replace("pipeline_dequant_mul_mat_vec_f32_f32[dmmv_wg][a_type][num_cols-1] : "
            "ctx->device->pipeline_dequant_mul_mat_vec_f16_f32[dmmv_wg]",
            "pipeline_dequant_mul_mat_vec_f32_f32[ggml_vk_ps5_dmmv_wg(dmmv_wg)][a_type][num_cols-1] : "
            "ctx->device->pipeline_dequant_mul_mat_vec_f16_f32[ggml_vk_ps5_dmmv_wg(dmmv_wg)]")
    replace("pipeline_dequant_mul_mat_vec_id_q8_1_f32[dmmv_wg]",
            "pipeline_dequant_mul_mat_vec_id_q8_1_f32[ggml_vk_ps5_dmmv_wg(dmmv_wg)]")
    replace("pipeline_dequant_mul_mat_vec_id_f32[dmmv_wg]",
            "pipeline_dequant_mul_mat_vec_id_f32[ggml_vk_ps5_dmmv_wg(dmmv_wg)]")

    # 5. How much work goes into one GPU submission, overridable.
    replace("    uint64_t flops_per_submit = std::min(flops_cap, ctx->last_total_flops / 40u);\n",
            """    uint64_t flops_per_submit = std::min(flops_cap, ctx->last_total_flops / 40u);
    if (const char * submit_env = getenv("GGML_VK_PS5_SUBMIT_MFLOP")) {
        flops_per_submit = strtoull(submit_env, nullptr, 10) * 1000000ULL;
    }
""")

    path.write_text(text)
    print(f"{path}: PS5 knobs applied")


if __name__ == "__main__":
    main()

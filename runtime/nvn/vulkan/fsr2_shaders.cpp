// AMD FSR 2's shaders (third_party/fsr2, v2.2.1) for its Vulkan backend: the replacement for its
// vk/shaders/ffx_fsr2_shaders_vk.cpp. AMD ships those as headers of precompiled permutations made by its own shader
// compiler at its build; here the GLSL sources are built in (fsr2_shaders.inc, from third_party/fsr2/shaders) and
// each permutation a pass asks for is compiled once with shaderc - already the backend's GLSL compiler - then kept.
// What the backend needs besides the SPIR-V, the names and bindings of the pass's storage images, sampled images and
// uniform buffers (it binds its resources by those names), is read from the SPIR-V itself (spv_reflect).
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../../../third_party/fsr2/vk/shaders/ffx_fsr2_shaders_vk.h"

#include <stdarg.h>
static void LOG(const char *fmt, ...) {  // (as runtime/rt.h rt_log: one write per line)
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(b, 1, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1, stderr);
}

static const struct { const char *name, *text; } k_sources[] = {
#include "fsr2_shaders.inc"
};

static shaderc_include_result *include_resolve(void *, const char *requested, int, const char *, size_t) {
    shaderc_include_result *r = (shaderc_include_result *)calloc(1, sizeof *r);
    for (const auto &s : k_sources)
        if (!strcmp(s.name, requested)) {
            r->source_name = s.name, r->source_name_length = strlen(s.name);
            r->content = s.text, r->content_length = strlen(s.text);
            return r;
        }
    static const char missing[] = "fsr2_shaders: no such include";
    r->content = missing, r->content_length = sizeof missing - 1;  // (an empty name: shaderc reports the error)
    return r;
}
static void include_release(void *, shaderc_include_result *r) { free(r); }

struct Blob {
    uint32_t pass, options;
    std::vector<uint32_t> spv;
    std::vector<std::string> names[3];  // storage images, sampled images, uniform buffers
    std::vector<const char *> cnames[3];
    std::vector<uint32_t> bindings[3];
};

// The descriptor set 1 variables of a SPIR-V module: [0] storage images, [1] sampled images, [2] uniform buffers.
// (Set 0 holds the backend's own samplers.)
static void spv_reflect(Blob &b) {
    const std::vector<uint32_t> &w = b.spv;
    size_t n = w.size();
    if (n < 5) return;
    uint32_t bound = w[3];
    std::vector<std::string> name(bound);
    std::vector<int32_t> binding(bound, -1), set(bound, -1);
    std::vector<uint32_t> op(bound), a1(bound), a2(bound), a3(bound);  // (type definitions: opcode and operands)
    struct Var { uint32_t id, type, sc; };
    std::vector<Var> vars;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, code = w[i] & 0xFFFF;
        if (!len || i + len > n) break;
        const uint32_t *o = &w[i + 1];
        switch (code) {
        case 5:  // OpName
            if (o[0] < bound) name[o[0]] = (const char *)&o[1];
            break;
        case 71:  // OpDecorate
            if (o[0] < bound && o[1] == 33) binding[o[0]] = (int32_t)o[2];
            if (o[0] < bound && o[1] == 34) set[o[0]] = (int32_t)o[2];
            break;
        case 25:  // OpTypeImage: result, sampled type, dim, depth, arrayed, ms, sampled (1 sampled, 2 storage), format
            if (o[0] < bound) op[o[0]] = 25, a1[o[0]] = o[6];
            break;
        case 28:  // OpTypeArray: result, element type
        case 29:  // OpTypeRuntimeArray
            if (o[0] < bound) op[o[0]] = 28, a1[o[0]] = o[1];
            break;
        case 30:  // OpTypeStruct
            if (o[0] < bound) op[o[0]] = 30;
            break;
        case 32:  // OpTypePointer: result, storage class, type
            if (o[0] < bound) op[o[0]] = 32, a2[o[0]] = o[1], a3[o[0]] = o[2];
            break;
        case 59:  // OpVariable: result type, result, storage class
            vars.push_back({o[1], o[0], o[2]});
            break;
        }
        i += len;
    }
    for (const Var &v : vars) {
        if (v.id >= bound || set[v.id] != 1 || binding[v.id] < 0 || v.type >= bound || op[v.type] != 32) continue;
        uint32_t t = a3[v.type];
        while (t < bound && op[t] == 28) t = a1[t];
        int kind = t >= bound ? -1 : op[t] == 25 ? (a1[t] == 2 ? 0 : 1) : op[t] == 30 ? 2 : -1;
        if (kind < 0) continue;
        b.names[kind].push_back(name[v.id]);
        b.bindings[kind].push_back((uint32_t)binding[v.id]);
    }
    for (int k = 0; k < 3; k++)
        for (const std::string &s : b.names[k]) b.cnames[k].push_back(s.c_str());
}

static const char *pass_source(FfxFsr2Pass pass) {
    switch (pass) {
    case FFX_FSR2_PASS_DEPTH_CLIP: return "ffx_fsr2_depth_clip_pass.glsl";
    case FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH: return "ffx_fsr2_reconstruct_previous_depth_pass.glsl";
    case FFX_FSR2_PASS_LOCK: return "ffx_fsr2_lock_pass.glsl";
    case FFX_FSR2_PASS_ACCUMULATE:
    case FFX_FSR2_PASS_ACCUMULATE_SHARPEN: return "ffx_fsr2_accumulate_pass.glsl";
    case FFX_FSR2_PASS_RCAS: return "ffx_fsr2_rcas_pass.glsl";
    case FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID: return "ffx_fsr2_compute_luminance_pyramid_pass.glsl";
    case FFX_FSR2_PASS_GENERATE_REACTIVE: return "ffx_fsr2_autogen_reactive_pass.glsl";
    case FFX_FSR2_PASS_TCR_AUTOGENERATE: return "ffx_fsr2_tcr_autogen_pass.glsl";
    default: return NULL;
    }
}

// (FSR2_SHADER_PERMUTATION_FORCE_WAVE64 picks another table in AMD's build, the same SPIR-V: ignored)
extern "C" Fsr2ShaderBlobVK fsr2GetPermutationBlobByIndexVK(FfxFsr2Pass pass, uint32_t options) {
    static std::vector<Blob *> cache;
    Fsr2ShaderBlobVK out = {};
    // (16-bit types: the device is made without shaderFloat16 / shaderInt16 - vulkan/device.c - so never those)
    options &= ~(uint32_t)(FSR2_SHADER_PERMUTATION_FORCE_WAVE64 | FSR2_SHADER_PERMUTATION_ALLOW_FP16);
    Blob *b = NULL;
    for (Blob *c : cache)
        if (c->pass == (uint32_t)pass && c->options == options) b = c;
    if (!b) {
        const char *file = pass_source(pass), *text = NULL;
        for (const auto &s : k_sources)
            if (file && !strcmp(s.name, file)) text = s.text;
        if (!text) { LOG("[fsr2] no shader for pass %d\n", (int)pass); return out; }
        shaderc_compiler_t comp = shaderc_compiler_initialize();
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
        // (unoptimized: shaderc's optimizer strips the names the backend binds by; the driver optimizes anyway)
        shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_zero);
        shaderc_compile_options_set_include_callbacks(o, include_resolve, include_release, NULL);
        struct { const char *d; uint32_t flag; } defs[] = {
            {"FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE", FSR2_SHADER_PERMUTATION_REPROJECT_USE_LANCZOS_TYPE},
            {"FFX_FSR2_OPTION_HDR_COLOR_INPUT", FSR2_SHADER_PERMUTATION_HDR_COLOR_INPUT},
            {"FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS", FSR2_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS},
            {"FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS", FSR2_SHADER_PERMUTATION_JITTER_MOTION_VECTORS},
            {"FFX_FSR2_OPTION_INVERTED_DEPTH", FSR2_SHADER_PERMUTATION_DEPTH_INVERTED},
            {"FFX_FSR2_OPTION_APPLY_SHARPENING", FSR2_SHADER_PERMUTATION_ENABLE_SHARPENING},
            {"FFX_HALF", FSR2_SHADER_PERMUTATION_ALLOW_FP16}};
        shaderc_compile_options_add_macro_definition(o, "FFX_GLSL", 8, "1", 1);
        shaderc_compile_options_add_macro_definition(o, "FFX_GPU", 7, "1", 1);
        for (const auto &d : defs) shaderc_compile_options_add_macro_definition(o, d.d, strlen(d.d), options & d.flag ? "1" : "0", 1);
        shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, text, strlen(text), shaderc_compute_shader, file, "main", o);
        bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
        if (ok) {
            b = new Blob;
            b->pass = (uint32_t)pass, b->options = options;
            size_t bytes = shaderc_result_get_length(r);
            b->spv.assign((const uint32_t *)shaderc_result_get_bytes(r), (const uint32_t *)shaderc_result_get_bytes(r) + bytes / 4);
            spv_reflect(*b);
            cache.push_back(b);
            if (getenv("LARECOMP_FSR2_REFLECT"))
                for (int k = 0; k < 3; k++)
                    for (size_t i = 0; i < b->names[k].size(); i++) LOG("[fsr2]   %c %u %s\n", "sic"[k], b->bindings[k][i], b->names[k][i].c_str());
            LOG("[fsr2] %s (options %#x): %zu bytes, %zu storage / %zu sampled images, %zu buffers\n", file, options, bytes,
                b->names[0].size(), b->names[1].size(), b->names[2].size());
        } else {
            LOG("[fsr2] %s (options %#x) failed to compile:\n%s\n", file, options, shaderc_result_get_error_message(r));
        }
        shaderc_result_release(r);
        shaderc_compile_options_release(o);
        shaderc_compiler_release(comp);
        if (!b) return out;
    }
    out.data = (const uint8_t *)b->spv.data();
    out.size = (uint32_t)(b->spv.size() * 4);
    out.storageImageCount = (uint32_t)b->names[0].size();
    out.sampledImageCount = (uint32_t)b->names[1].size();
    out.uniformBufferCount = (uint32_t)b->names[2].size();
    out.boundStorageImageNames = b->cnames[0].data(), out.boundStorageImageBindings = b->bindings[0].data();
    out.boundSampledImageNames = b->cnames[1].data(), out.boundSampledImageBindings = b->bindings[1].data();
    out.boundUniformBufferNames = b->cnames[2].data(), out.boundUniformBufferBindings = b->bindings[2].data();
    return out;
}

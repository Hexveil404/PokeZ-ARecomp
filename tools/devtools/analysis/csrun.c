// csrun: runs one captured compute dispatch again on this machine's GPU (#345: the culling dispatches that wrote over
// AMD's indirect parameters - run here on NVIDIA with the same inputs, a different result says AMD computed them wrong,
// the same one says the inputs were bad). Written for csrun.py, which turns a capture (logs/gpuw-*.bin, fullcap's
// 'DISB' / 'DISA' records) into a job folder; this reads <job>/job.txt:
//   glsl <file>                         the shader (csrun.py patched its guest window base)
//   local <x> <y> <z>                   specialization constants 0..2
//   groups <x> <y> <z>
//   ubo <binding> <file>                uniform buffer contents
//   ssbo <binding> <file>               storage buffer; written back to <file>.out after the dispatch
//   tex <binding> <vkformat> <w> <h> <levels> <depth 0/1> <file> <minfilter> <magfilter> <mipmode> <addrmode> <reduction> <minlod> <maxlod>
// Build (MSYS2 UCRT64): gcc -O1 -o csrun.exe csrun.c -lvulkan-1 -lshaderc_shared
#include <shaderc/shaderc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define MAXB 64

static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue q;
static uint32_t qf;
static VkCommandPool pool;

static void *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "can't read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n) exit(1);
    ((char *)p)[*n] = 0;
    fclose(f);
    return p;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize size; } Buf;
static Buf buf_new(VkDeviceSize size, VkBufferUsageFlags use) {  // host-visible, coherent (small jobs: simplicity first)
    Buf r = {.size = size};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use};
    CK(vkCreateBuffer(dev, &bi, NULL, &r.b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(dev, &ai, NULL, &r.m));
    CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
    memset(r.p, 0, size);
    return r;
}

static VkCommandBuffer cmd_begin(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}
static void cmd_run(VkCommandBuffer cb) {
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: csrun <job folder>\n"); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s/job.txt", argv[1]);
    FILE *jf = fopen(path, "r");
    if (!jf) { fprintf(stderr, "no %s\n", path); return 1; }

    // ---- the device: the first discrete GPU (or the first one)
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t np = 8;
    VkPhysicalDevice pds[8];
    CK(vkEnumeratePhysicalDevices(inst, &np, pds));
    phys = pds[0];
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pds[i]; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    fprintf(stderr, "[csrun] %s\n", props.deviceName);
    uint32_t nq = 16;
    VkQueueFamilyProperties qfp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qfp);
    for (qf = 0; qf < nq && !(qfp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT); qf++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .samplerFilterMinmax = VK_TRUE,
                                           .shaderInt8 = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE, .scalarBlockLayout = VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12, .subgroupSizeControl = VK_TRUE};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
                                    .features = {.shaderInt64 = VK_TRUE, .shaderInt16 = VK_TRUE, .robustBufferAccess = VK_TRUE,
                                                 .shaderImageGatherExtended = VK_TRUE, .fragmentStoresAndAtomics = VK_TRUE}};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    CK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf};
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    // ---- the job
    char line[2048], glsl[1024] = "";
    uint32_t local[3] = {1, 1, 1}, groups[3] = {1, 1, 1};
    VkDescriptorSetLayoutBinding lb[MAXB];
    VkWriteDescriptorSet wr[MAXB];
    VkDescriptorBufferInfo dbi[MAXB];
    VkDescriptorImageInfo dii[MAXB];
    Buf ssbo[MAXB];
    char ssbo_file[MAXB][1024];
    int nssbo = 0, nb = 0;
    while (fgets(line, sizeof line, jf)) {
        char kind[16], file[1024];
        unsigned b;
        if (sscanf(line, "glsl %1023s", glsl) == 1) continue;
        if (sscanf(line, "local %u %u %u", &local[0], &local[1], &local[2]) == 3) continue;
        if (sscanf(line, "groups %u %u %u", &groups[0], &groups[1], &groups[2]) == 3) continue;
        if (sscanf(line, "%15s %u %1023s", kind, &b, file) == 3 && (!strcmp(kind, "ubo") || !strcmp(kind, "ssbo"))) {
            bool ub = !strcmp(kind, "ubo");
            char fp[1100];
            snprintf(fp, sizeof fp, "%s/%s", argv[1], file);
            size_t n;
            void *data = slurp(fp, &n);
            Buf bf = buf_new(n < 16 ? 16 : n, ub ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            memcpy(bf.p, data, n);
            free(data);
            VkDescriptorType t = ub ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            lb[nb] = (VkDescriptorSetLayoutBinding){b, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            dbi[nb] = (VkDescriptorBufferInfo){bf.b, 0, VK_WHOLE_SIZE};
            wr[nb] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b, .descriptorCount = 1,
                                            .descriptorType = t, .pBufferInfo = &dbi[nb]};
            if (!ub) { ssbo[nssbo] = bf; snprintf(ssbo_file[nssbo++], sizeof ssbo_file[0], "%s.out", fp); }
            nb++;
            continue;
        }
        unsigned fmt, w, h, lv, depth, minf, magf, mipm, addr, red;
        float lmin, lmax;
        if (sscanf(line, "tex %u %u %u %u %u %u %1023s %u %u %u %u %u %f %f", &b, &fmt, &w, &h, &lv, &depth, file, &minf, &magf, &mipm,
                   &addr, &red, &lmin, &lmax) == 14) {
            char fp[1100];
            snprintf(fp, sizeof fp, "%s/%s", argv[1], file);
            size_t n;
            void *data = slurp(fp, &n);
            VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = (VkFormat)fmt,
                                    .extent = {w, h, 1}, .mipLevels = lv, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
            VkImage img;
            CK(vkCreateImage(dev, &ii, NULL, &img));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev, img, &mr);
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                       .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VkDeviceMemory im;
            CK(vkAllocateMemory(dev, &ai, NULL, &im));
            CK(vkBindImageMemory(dev, img, im, 0));
            Buf st = buf_new(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            memcpy(st.p, data, n);
            free(data);
            VkImageAspectFlags asp = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkCommandBuffer cb = cmd_begin();
            VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : asp, 0, lv, 0, 1}};
            if (depth && fmt == VK_FORMAT_D32_SFLOAT) ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            VkBufferImageCopy c[16];
            VkDeviceSize off = 0;
            uint32_t tx = (uint32_t)(n / 1);  // (texel size: from the level sizes)
            uint64_t texels = 0;
            for (uint32_t l = 0; l < lv; l++) texels += (uint64_t)(w >> l ? w >> l : 1) * (h >> l ? h >> l : 1);
            tx = (uint32_t)(n / texels);
            for (uint32_t l = 0; l < lv && l < 16; l++) {
                uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
                c[l] = (VkBufferImageCopy){.bufferOffset = off, .imageSubresource = {asp, l, 0, 1}, .imageExtent = {lw, lh, 1}};
                off += (VkDeviceSize)lw * lh * tx;
            }
            vkCmdCopyBufferToImage(cb, st.b, img, VK_IMAGE_LAYOUT_GENERAL, lv < 16 ? lv : 16, c);
            barrier(cb);
            cmd_run(cb);
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = (VkFormat)fmt, .subresourceRange = {asp, 0, lv, 0, 1}};
            VkImageView view;
            CK(vkCreateImageView(dev, &vi, NULL, &view));
            VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = (VkSamplerReductionMode)red};
            VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = red ? &rm : NULL, .magFilter = (VkFilter)magf,
                                      .minFilter = (VkFilter)minf, .mipmapMode = (VkSamplerMipmapMode)mipm,
                                      .addressModeU = (VkSamplerAddressMode)addr, .addressModeV = (VkSamplerAddressMode)addr,
                                      .addressModeW = (VkSamplerAddressMode)addr, .minLod = lmin, .maxLod = lmax};
            VkSampler smp;
            CK(vkCreateSampler(dev, &si, NULL, &smp));
            lb[nb] = (VkDescriptorSetLayoutBinding){b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            dii[nb] = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
            wr[nb] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b, .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[nb]};
            nb++;
            continue;
        }
    }
    fclose(jf);

    // ---- the shader
    char gp[1100];
    snprintf(gp, sizeof gp, "%s/%s", argv[1], glsl);
    size_t gn;
    char *src = slurp(gp, &gn);
    shaderc_compiler_t sc = shaderc_compiler_initialize();
    shaderc_compile_options_t so = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(so, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(so, shaderc_optimization_level_zero);  // (as the runtime: the driver optimizes)
    shaderc_compilation_result_t res = shaderc_compile_into_spv(sc, src, gn, shaderc_compute_shader, glsl, "main", so);
    if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
        fprintf(stderr, "GLSL: %s\n", shaderc_result_get_error_message(res));
        return 1;
    }
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaderc_result_get_length(res),
                                    .pCode = (const uint32_t *)shaderc_result_get_bytes(res)};
    VkShaderModule mod;
    CK(vkCreateShaderModule(dev, &mci, NULL, &mod));

    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nb, .pBindings = lb};
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, local};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req32 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
                                                                 .requiredSubgroupSize = 32};
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &req32,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main", .pSpecializationInfo = &spec},
                                        .layout = pl};
    VkPipeline pipe;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAXB}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAXB}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAXB}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps};
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    for (int i = 0; i < nb; i++) wr[i].dstSet = ds;
    vkUpdateDescriptorSets(dev, (uint32_t)nb, wr, 0, NULL);

    VkCommandBuffer cb = cmd_begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, groups[0], groups[1], groups[2]);
    barrier(cb);
    cmd_run(cb);
    for (int i = 0; i < nssbo; i++) {
        FILE *f = fopen(ssbo_file[i], "wb");
        fwrite(ssbo[i].p, 1, ssbo[i].size, f);
        fclose(f);
    }
    fprintf(stderr, "[csrun] ran %ux%ux%u groups of %ux%ux%u\n", groups[0], groups[1], groups[2], local[0], local[1], local[2]);
    return 0;
}

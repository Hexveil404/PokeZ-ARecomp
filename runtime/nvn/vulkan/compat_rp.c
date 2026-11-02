// Vulkan 1.2 GPUs without dynamic rendering (compat.c, vk.h VkCompat.render_pass): vkCmdBeginRendering becomes
// vkCmdBeginRenderPass - a render pass made from the attachments' formats, sample counts, load / store ops and layouts,
// and a framebuffer of the views (both cached). A pipeline made for dynamic rendering (VkPipelineRenderingCreateInfo)
// gets a compatible render pass instead (render pass compatibility is formats and sample counts only).
// The views' formats and sample counts, which VkRenderingInfo doesn't carry, are noted as views and images are made
// (vkc_CreateImageView, vkc_vmaCreateImage), and a view's framebuffers go with it (the backend destroys a view once
// the GPU is done with it, so its framebuffers aren't in use either).
#include <pthread.h>
#include <string.h>

#include "vk.h"
#undef vkCreateImageView
#undef vkDestroyImageView
#undef vmaCreateImage
#undef vkCreateGraphicsPipelines

static pthread_mutex_t rp_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- image -> samples, view -> format + samples (open addressing; a removed entry keeps key 1, a tombstone)
#define REG_TAB (1u << 16)
static struct { uint64_t key; VkSampleCountFlagBits samples; } img_tab[REG_TAB];
static struct { uint64_t key; VkFormat format; VkSampleCountFlagBits samples; } view_tab[REG_TAB];
static uint32_t reg_slot(uint64_t k) { k *= 0x9E3779B97F4A7C15ull; return (uint32_t)(k >> 40) & (REG_TAB - 1); }

static VkSampleCountFlagBits image_samples(VkImage img) {
    uint64_t k = (uint64_t)img;
    for (uint32_t i = reg_slot(k), n = 0; n < REG_TAB && img_tab[i].key; i = (i + 1) & (REG_TAB - 1), n++)
        if (img_tab[i].key == k) return img_tab[i].samples;
    return VK_SAMPLE_COUNT_1_BIT;  // (not noted: single-sampled, or a swapchain image)
}

VkResult vkc_vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                            VmaAllocation *mem, VmaAllocationInfo *info) {
    VkResult r = vmaCreateImage(a, ci, ai, img, mem, info);
    if (r != VK_SUCCESS || !g_vkc.render_pass || ci->samples == VK_SAMPLE_COUNT_1_BIT) return r;
    pthread_mutex_lock(&rp_lock);  // (only multisampled images are noted)
    uint64_t k = (uint64_t)*img;
    uint32_t i = reg_slot(k);
    while (img_tab[i].key > 1 && img_tab[i].key != k) i = (i + 1) & (REG_TAB - 1);
    img_tab[i].key = k, img_tab[i].samples = ci->samples;
    pthread_mutex_unlock(&rp_lock);
    return r;
}

static bool view_info(VkImageView v, VkFormat *f, VkSampleCountFlagBits *s) {
    uint64_t k = (uint64_t)v;
    for (uint32_t i = reg_slot(k), n = 0; n < REG_TAB && view_tab[i].key; i = (i + 1) & (REG_TAB - 1), n++)
        if (view_tab[i].key == k) { *f = view_tab[i].format, *s = view_tab[i].samples; return true; }
    return false;
}

VkResult vkc_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *al, VkImageView *v) {
    VkResult r = vkCreateImageView(dev, ci, al, v);
    if (r != VK_SUCCESS || !g_vkc.render_pass) return r;
    pthread_mutex_lock(&rp_lock);
    uint64_t k = (uint64_t)*v;
    uint32_t i = reg_slot(k);
    while (view_tab[i].key > 1 && view_tab[i].key != k) i = (i + 1) & (REG_TAB - 1);
    view_tab[i].key = k, view_tab[i].format = ci->format, view_tab[i].samples = image_samples(ci->image);
    pthread_mutex_unlock(&rp_lock);
    return r;
}

// ---- framebuffers: (render pass, views, size, layers) -> VkFramebuffer
#define FB_TAB 4096
typedef struct { VkRenderPass rp; VkImageView v[9]; uint32_t n, w, h, layers; } FbKey;
static struct { FbKey k; VkFramebuffer fb; bool used, ever; } fb_tab[FB_TAB];
static int nfb;

void vkc_DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *al) {
    if (g_vkc.render_pass && v) {
        pthread_mutex_lock(&rp_lock);
        uint64_t k = (uint64_t)v;
        for (uint32_t i = reg_slot(k), n = 0; n < REG_TAB && view_tab[i].key; i = (i + 1) & (REG_TAB - 1), n++)
            if (view_tab[i].key == k) { view_tab[i].key = 1; break; }
        for (int i = 0; i < FB_TAB && nfb; i++) {
            if (!fb_tab[i].used) continue;
            for (uint32_t a = 0; a < fb_tab[i].k.n; a++)
                if (fb_tab[i].k.v[a] == v) {
                    vkDestroyFramebuffer(dev, fb_tab[i].fb, NULL);
                    fb_tab[i].used = false, nfb--;
                    break;
                }
        }
        pthread_mutex_unlock(&rp_lock);
    }
    vkDestroyImageView(dev, v, al);
}

// ---- render passes: one subpass, colour attachments 0..n-1 (missing ones VK_ATTACHMENT_UNUSED), depth / stencil last
typedef struct {
    uint32_t ncolor;
    VkFormat color[8], ds;
    VkSampleCountFlagBits samples;
    uint8_t cload[8], cstore[8], dload, dstore, sload, sstore;
    VkImageLayout clayout[8], dslayout;
} RpKey;
#define RP_TAB 1024
static struct { RpKey k; VkRenderPass rp; } rp_tab[RP_TAB];
static int nrp;

static VkRenderPass render_pass(const RpKey *k) {  // (rp_lock held)
    for (int i = 0; i < nrp; i++) if (!memcmp(&rp_tab[i].k, k, sizeof *k)) return rp_tab[i].rp;
    VkAttachmentDescription ad[9];
    VkAttachmentReference cr[8], dr;
    uint32_t na = 0;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!k->color[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}; continue; }
        ad[na] = (VkAttachmentDescription){.format = k->color[i], .samples = k->samples,
                                           .loadOp = (VkAttachmentLoadOp)k->cload[i], .storeOp = (VkAttachmentStoreOp)k->cstore[i],
                                           .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                           .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           .initialLayout = k->clayout[i], .finalLayout = k->clayout[i]};
        cr[i] = (VkAttachmentReference){na++, k->clayout[i]};
    }
    if (k->ds) {
        ad[na] = (VkAttachmentDescription){.format = k->ds, .samples = k->samples, .loadOp = (VkAttachmentLoadOp)k->dload,
                                           .storeOp = (VkAttachmentStoreOp)k->dstore,
                                           .stencilLoadOp = (VkAttachmentLoadOp)k->sload,
                                           .stencilStoreOp = (VkAttachmentStoreOp)k->sstore,
                                           .initialLayout = k->dslayout, .finalLayout = k->dslayout};
        dr = (VkAttachmentReference){na++, k->dslayout};
    }
    VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor,
                               .pColorAttachments = cr, .pDepthStencilAttachment = k->ds ? &dr : NULL};
    // (the backend puts full barriers around its passes; these keep the external dependencies as wide as those)
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = ad,
                                 .subpassCount = 1, .pSubpasses = &sd, .dependencyCount = 2, .pDependencies = dep};
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp));
    if (nrp < RP_TAB) rp_tab[nrp].k = *k, rp_tab[nrp++].rp = rp;
    else { static int n; if (!n++) LOG("[vk] compat: more than %d render passes\n", RP_TAB); }
    return rp;
}

VkRenderPass vkc_render_pass_for(uint32_t ncolor, const VkFormat *color, VkFormat depth, VkFormat stencil,
                                 VkSampleCountFlagBits samples, VkImageLayout layout) {
    RpKey k;
    memset(&k, 0, sizeof k);
    k.ncolor = ncolor > 8 ? 8 : ncolor;
    k.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        k.color[i] = color ? color[i] : VK_FORMAT_UNDEFINED;
        k.cload[i] = VK_ATTACHMENT_LOAD_OP_LOAD, k.cstore[i] = VK_ATTACHMENT_STORE_OP_STORE, k.clayout[i] = layout;
    }
    k.ds = depth ? depth : stencil;
    if (k.ds) {
        k.dload = k.sload = VK_ATTACHMENT_LOAD_OP_LOAD, k.dstore = k.sstore = VK_ATTACHMENT_STORE_OP_STORE;
        k.dslayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    pthread_mutex_lock(&rp_lock);
    VkRenderPass rp = render_pass(&k);
    pthread_mutex_unlock(&rp_lock);
    return rp;
}

void vkc_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (!g_vkc.render_pass) { vkp_vkCmdBeginRendering(cb, ri); return; }
    RpKey k;
    memset(&k, 0, sizeof k);
    FbKey fk;
    memset(&fk, 0, sizeof fk);
    VkClearValue cv[9];
    uint32_t na = 0;
    k.ncolor = ri->colorAttachmentCount > 8 ? 8 : ri->colorAttachmentCount;
    k.samples = VK_SAMPLE_COUNT_1_BIT;
    pthread_mutex_lock(&rp_lock);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        VkFormat f;
        VkSampleCountFlagBits s;
        if (!a->imageView || !view_info(a->imageView, &f, &s)) continue;
        k.color[i] = f, k.samples = s;
        k.cload[i] = (uint8_t)a->loadOp, k.cstore[i] = (uint8_t)a->storeOp, k.clayout[i] = a->imageLayout;
        cv[na] = a->clearValue;
        fk.v[na++] = a->imageView;
    }
    const VkRenderingAttachmentInfo *d = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *s = ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment : NULL;
    const VkRenderingAttachmentInfo *ds = d ? d : s;  // (the backend gives depth and stencil the same view)
    VkFormat df;
    VkSampleCountFlagBits dsm;
    if (ds && view_info(ds->imageView, &df, &dsm)) {
        k.ds = df, k.samples = dsm, k.dslayout = ds->imageLayout;
        k.dload = (uint8_t)(d ? d->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        k.dstore = (uint8_t)(d ? d->storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE);
        k.sload = (uint8_t)(s ? s->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        k.sstore = (uint8_t)(s ? s->storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE);
        cv[na].depthStencil = (VkClearDepthStencilValue){d ? d->clearValue.depthStencil.depth : 0.0f,
                                                         s ? s->clearValue.depthStencil.stencil : 0};
        fk.v[na++] = ds->imageView;
    }
    VkRenderPass rp = render_pass(&k);
    fk.rp = rp, fk.n = na;
    fk.w = (uint32_t)ri->renderArea.offset.x + ri->renderArea.extent.width;
    fk.h = (uint32_t)ri->renderArea.offset.y + ri->renderArea.extent.height;
    fk.layers = ri->layerCount ? ri->layerCount : 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof fk; i++) { h ^= ((const uint8_t *)&fk)[i]; h *= 0x100000001B3ull; }
    uint32_t slot = (uint32_t)(h >> 40) & (FB_TAB - 1), free_slot = UINT32_MAX;
    for (uint32_t n = 0; n < FB_TAB; n++, slot = (slot + 1) & (FB_TAB - 1)) {
        if (fb_tab[slot].used) {
            if (!memcmp(&fb_tab[slot].k, &fk, sizeof fk)) { fb = fb_tab[slot].fb; break; }
            continue;
        }
        if (free_slot == UINT32_MAX) free_slot = slot;
        if (!fb_tab[slot].ever) break;  // (never used: the probe sequence ends here)
    }
    if (!fb) {
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = na,
                                      .pAttachments = fk.v, .width = fk.w, .height = fk.h, .layers = fk.layers};
        VK_CHECK(vkCreateFramebuffer(g_vk.dev, &fi, NULL, &fb));
        if (free_slot != UINT32_MAX) {
            fb_tab[free_slot].k = fk, fb_tab[free_slot].fb = fb, fb_tab[free_slot].used = fb_tab[free_slot].ever = true;
            nfb++;
        } else {
            static int n;
            if (!n++) LOG("[vk] compat: framebuffer table full\n");  // (that framebuffer then leaks)
        }
    }
    pthread_mutex_unlock(&rp_lock);
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                .renderArea = ri->renderArea, .clearValueCount = na, .pClearValues = cv};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void vkc_CmdEndRendering(VkCommandBuffer cb) {
    if (!g_vkc.render_pass) { vkp_vkCmdEndRendering(cb); return; }
    vkCmdEndRenderPass(cb);
}

VkResult vkc_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                     const VkAllocationCallbacks *al, VkPipeline *out) {
    if (!g_vkc.render_pass) return vkCreateGraphicsPipelines(dev, cache, n, ci, al, out);
    for (uint32_t i = 0; i < n; i++) {
        VkGraphicsPipelineCreateInfo c = ci[i];
        // (anywhere in the chain: exec.c's SLOW_FEEDBACK goes in front of it, and looking only at the first entry left every
        // pipeline of the game with no render pass - invalid, which NVIDIA forgives and an old Intel driver doesn't: the
        // menus drawn by other pipelines showed, the world didn't, #112. LARECOMP_VK_VALIDATION=1 with
        // LARECOMP_VK_INTEL_RENDER_PASSES=1 tells: VUID-VkGraphicsPipelineCreateInfo-dynamicRendering-06576)
        VkBaseOutStructure *prev = NULL, *e = (VkBaseOutStructure *)c.pNext;
        while (e && e->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) prev = e, e = e->pNext;
        const VkPipelineRenderingCreateInfo *r = (const VkPipelineRenderingCreateInfo *)e;
        bool unlinked = false;
        if (r && !c.renderPass) {
            VkSampleCountFlagBits s = c.pMultisampleState ? c.pMultisampleState->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;
            c.renderPass = vkc_render_pass_for(r->colorAttachmentCount, r->pColorAttachmentFormats, r->depthAttachmentFormat,
                                               r->stencilAttachmentFormat, s, VK_IMAGE_LAYOUT_GENERAL);
            c.subpass = 0;
            if (prev) prev->pNext = e->pNext, unlinked = true;  // (the caller's chain: put back below)
            else c.pNext = r->pNext;
        }
        VkResult res = vkCreateGraphicsPipelines(dev, cache, 1, &c, al, &out[i]);
        if (unlinked) prev->pNext = e;
        if (res != VK_SUCCESS) return res;
    }
    return VK_SUCCESS;
}

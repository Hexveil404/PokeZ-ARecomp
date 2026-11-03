// Vulkan 1.2 GPUs: what the backend uses from Vulkan 1.3, done with what an older driver has (vk.h VkCompat).
// The backend is written against Vulkan 1.3; the commands below stand in for the 1.3 ones it calls, and take the
// 1.3 path whenever the driver has it.
//
// synchronization2 (vkCmdPipelineBarrier2, vkQueueSubmit2, vkCmdWriteTimestamp2) -> the Vulkan 1.0 commands. The
// 64-bit stage / access masks keep their low 32 bits (the same bits as Vulkan 1.0's); a stage or access that only
// exists in the 64-bit masks becomes ALL_COMMANDS / MEMORY_READ / MEMORY_WRITE - wider, never narrower.
#include <stdlib.h>
#include <string.h>

#include "vk.h"

VkCompat g_vkc;

// ================================================================== synchronization2
static VkPipelineStageFlags stage1(VkPipelineStageFlags2 s, bool src) {
    if (!s) return src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;  // (NONE)
    if (s >> 32) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;  // (copy / blit / clear / index input / pre-raster...)
    return (VkPipelineStageFlags)s;
}

static VkAccessFlags access1(VkAccessFlags2 a) {
    VkAccessFlags r = (VkAccessFlags)(a & 0xFFFFFFFFull);
    if (a >> 32) {
        const VkAccessFlags2 reads = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const VkAccessFlags2 writes = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (a & reads) r |= VK_ACCESS_SHADER_READ_BIT;
        if (a & writes) r |= VK_ACCESS_SHADER_WRITE_BIT;
        if (a & ~(reads | writes) & ~0xFFFFFFFFull) r |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    return r;
}

void vkc_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d) {
    // (a combined depth + stencil image: both aspects, compat_rp.c vkc_image_is_ds)
    VkImageMemoryBarrier2 fixed[32];
    VkDependencyInfo dd;
    for (uint32_t i = 0; i < d->imageMemoryBarrierCount && i < 32; i++) {
        VkImageAspectFlags as = d->pImageMemoryBarriers[i].subresourceRange.aspectMask;
        const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!(as & ds) || (as & ds) == ds || !vkc_image_is_ds(d->pImageMemoryBarriers[i].image)) continue;
        if (d != &dd) {
            memcpy(fixed, d->pImageMemoryBarriers, (d->imageMemoryBarrierCount < 32 ? d->imageMemoryBarrierCount : 32) * sizeof *fixed);
            dd = *d, dd.pImageMemoryBarriers = fixed, d = &dd;
        }
        fixed[i].subresourceRange.aspectMask |= ds;
    }
    if (!g_vkc.sync1) { vkp_vkCmdPipelineBarrier2(cb, d); return; }
    VkPipelineStageFlags2 src = 0, dst = 0;
    VkMemoryBarrier mb[8];
    VkBufferMemoryBarrier bb[8];
    VkImageMemoryBarrier ib[16];
    uint32_t nm = d->memoryBarrierCount < 8 ? d->memoryBarrierCount : 8;
    uint32_t nb = d->bufferMemoryBarrierCount < 8 ? d->bufferMemoryBarrierCount : 8;
    uint32_t ni = d->imageMemoryBarrierCount < 16 ? d->imageMemoryBarrierCount : 16;
    for (uint32_t i = 0; i < nm; i++) {
        const VkMemoryBarrier2 *m = &d->pMemoryBarriers[i];
        src |= m->srcStageMask, dst |= m->dstStageMask;
        mb[i] = (VkMemoryBarrier){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = access1(m->srcAccessMask),
                                  .dstAccessMask = access1(m->dstAccessMask)};
    }
    for (uint32_t i = 0; i < nb; i++) {
        const VkBufferMemoryBarrier2 *b = &d->pBufferMemoryBarriers[i];
        src |= b->srcStageMask, dst |= b->dstStageMask;
        bb[i] = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = access1(b->srcAccessMask), .dstAccessMask = access1(b->dstAccessMask),
                                        .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
                                        .dstQueueFamilyIndex = b->dstQueueFamilyIndex, .buffer = b->buffer,
                                        .offset = b->offset, .size = b->size};
    }
    for (uint32_t i = 0; i < ni; i++) {
        const VkImageMemoryBarrier2 *m = &d->pImageMemoryBarriers[i];
        src |= m->srcStageMask, dst |= m->dstStageMask;
        ib[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .srcAccessMask = access1(m->srcAccessMask), .dstAccessMask = access1(m->dstAccessMask),
                                       .oldLayout = m->oldLayout, .newLayout = m->newLayout,
                                       .srcQueueFamilyIndex = m->srcQueueFamilyIndex,
                                       .dstQueueFamilyIndex = m->dstQueueFamilyIndex, .image = m->image,
                                       .subresourceRange = m->subresourceRange};
    }
    vkCmdPipelineBarrier(cb, stage1(src, true), stage1(dst, false), d->dependencyFlags, nm, mb, nb, bb, ni, ib);
}

VkResult vkc_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence) {
    if (!g_vkc.sync1) return vkp_vkQueueSubmit2(q, n, si, fence);
    for (uint32_t k = 0; k < n; k++) {  // (one VkSubmitInfo at a time: the backend submits one)
        const VkSubmitInfo2 *s = &si[k];
        VkCommandBuffer cbs[8];
        VkSemaphore ws[4], ss[4];
        uint64_t wv[4], sv[4];
        VkPipelineStageFlags wst[4];
        uint32_t ncb = s->commandBufferInfoCount < 8 ? s->commandBufferInfoCount : 8;
        uint32_t nw = s->waitSemaphoreInfoCount < 4 ? s->waitSemaphoreInfoCount : 4;
        uint32_t ns = s->signalSemaphoreInfoCount < 4 ? s->signalSemaphoreInfoCount : 4;
        for (uint32_t i = 0; i < ncb; i++) cbs[i] = s->pCommandBufferInfos[i].commandBuffer;
        for (uint32_t i = 0; i < nw; i++) {
            ws[i] = s->pWaitSemaphoreInfos[i].semaphore, wv[i] = s->pWaitSemaphoreInfos[i].value;
            wst[i] = stage1(s->pWaitSemaphoreInfos[i].stageMask, false);
        }
        for (uint32_t i = 0; i < ns; i++) ss[i] = s->pSignalSemaphoreInfos[i].semaphore, sv[i] = s->pSignalSemaphoreInfos[i].value;
        // (binary semaphores ignore their values; the timeline one takes its value from here)
        VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .waitSemaphoreValueCount = nw, .pWaitSemaphoreValues = wv,
                                            .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv};
        VkSubmitInfo s1 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl, .waitSemaphoreCount = nw,
                           .pWaitSemaphores = ws, .pWaitDstStageMask = wst, .commandBufferCount = ncb,
                           .pCommandBuffers = cbs, .signalSemaphoreCount = ns, .pSignalSemaphores = ss};
        VkResult r = vkQueueSubmit(q, 1, &s1, k + 1 == n ? fence : VK_NULL_HANDLE);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

void vkc_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    if (!g_vkc.sync1) { vkp_vkCmdWriteTimestamp2(cb, stage, pool, query); return; }
    vkCmdWriteTimestamp(cb, stage ? stage1(stage, false) : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, query);
}

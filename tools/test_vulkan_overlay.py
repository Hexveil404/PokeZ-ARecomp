"""The overlay's command buffer (device.c vk_overlay_begin / vk_overlay_end): ImGui records into a secondary command
buffer of device.c's while the recorder thread is on, and the window image's command buffer only after a drain. No GPU
is needed.

Extracts the overlay block from runtime/nvn/vulkan/device.c by its markers, compiles it with stubs that log the calls,
and checks: no per-frame drain with the recorder on; the order the recorder gets (end the window's rendering, begin a
rendering of secondary command buffers on the same image, execute); a slot's pool reset only after the GPU finished the
submission that executed it; a second overlay in one frame, the recorder off and LARECOMP_VK_OVERLAY_DIRECT=1 record
into the window image's command buffer after a drain, inline draws in a rendering of their own.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--ref', help='test an older Git revision to reproduce the failure')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
path = 'runtime/nvn/vulkan/device.c'
source = subprocess.check_output(['git', 'show', f'{args.ref}:{path}'], cwd=root, text=True) if args.ref else (root / path).read_text()
block = source[source.index('// ---- the overlay (menu.mm menu_render'):source.index('static void save_window_image(')]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#define setenv(k, v, o) _putenv_s(k, v)  // (MinGW has no setenv)
#endif
#define FRAMES 3
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define VK_CHECK(x) do { VkResult r_ = (x); assert(r_ == VK_SUCCESS); } while (0)
static struct { VkDevice dev; uint32_t qfam; VkFormat sc_format; VkExtent2D sc_extent; VkImageView sc_views[8]; } g_vk = {
    (VkDevice)0x10, 0, VK_FORMAT_B8G8R8A8_UNORM, {1280, 800}, {(VkImageView)0x100, (VkImageView)0x101}};
typedef struct { uint32_t image; VkSemaphore acquired; bool ok, secondary; uint64_t id; } Frame;
static Frame cur_frame;
static struct { bool render_pass; } g_vkc;  // (ZA Recomp: Vulkan 1.2 render passes draw the overlay directly - dynamic rendering here)
static int slot;
static bool recorder_on;
static uint64_t pending = 1, completed, waited_for;
static char trace[4096];
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
static bool rec_on(void) { return recorder_on; }
static void rec_drain(void) { note("drain"); }
static uint64_t vk_pending_seq(void) { return pending; }
static uint64_t vk_completed_seq(void) { return completed; }
static void vk_wait_seq(uint64_t s) { note("gpu-wait"); waited_for = s; if (completed < s) completed = s; }
static void vk_frame_suspend(void) { note("end"); }
static void vk_frame_resume(void) { note("begin-inline"); cur_frame.secondary = false; }
static void rec_CmdEndRendering(VkCommandBuffer cb) { assert(cb == (VkCommandBuffer)0x500); note("end"); }
static VkRenderingFlags begun_flags;
static void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    assert(cb == (VkCommandBuffer)0x500);
    assert(ri->colorAttachmentCount == 1 && ri->pColorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    assert(ri->pColorAttachments[0].imageView == g_vk.sc_views[cur_frame.image]);
    assert(ri->renderArea.extent.width == 1280 && ri->renderArea.extent.height == 800);
    begun_flags = ri->flags;
    note(ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT ? "begin-secondary" : "begin-inline");
}
static VkCommandBuffer executed;
static void rec_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) {
    assert(cb == (VkCommandBuffer)0x500 && n == 1);
    executed = cbs[0];
    note("execute");
}
// the driver: pools and secondary buffers (state: 0 initial, 1 recording, 2 executable)
static int pools, resets, cb_state[8];
static uintptr_t next_cb = 0x900;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *p) {
    (void)d; (void)a;
    assert(ci->flags == VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
    *p = (VkCommandPool)(uintptr_t)(0x700 + pools++);
    note("create-pool");
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) {
    (void)d;
    assert(ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && ai->commandBufferCount == 1);
    *cb = (VkCommandBuffer)next_cb++;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)p; (void)a; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    (void)d; (void)f;
    int i = (int)((uintptr_t)p - 0x700);
    assert(cb_state[i] == 2);  // (only a buffer that was recorded is reset again)
    cb_state[i] = 0;
    resets++;
    note("reset-pool");
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    int i = (int)((uintptr_t)cb - 0x900);
    assert(cb_state[i] == 0);
    assert(bi->flags == (VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT));
    const VkCommandBufferInheritanceRenderingInfo *ir = bi->pInheritanceInfo->pNext;
    assert(ir->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO && ir->colorAttachmentCount == 1);
    assert(ir->pColorAttachmentFormats[0] == g_vk.sc_format && ir->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT);
    assert(ir->flags == 0);  // (the rendering's flags but the contents bit)
    cb_state[i] = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) {
    int i = (int)((uintptr_t)cb - 0x900);
    assert(cb_state[i] == 1);
    cb_state[i] = 2;
    note("end-secondary");
    return VK_SUCCESS;
}
''' + block + r'''
static void *const window = (void *)0x500;
static void frame_begin(void) {  // (vk_frame_begin)
    slot = (slot + 1) % FRAMES;
    cur_frame.ok = true;
    cur_frame.id++;
    cur_frame.secondary = false;
    cur_frame.image = (uint32_t)(cur_frame.id & 1);
}
static void frame_end(void) { pending++; cur_frame.ok = false; }  // (vk_frame_end: the window submission)
static const char *overlay(void) {  // menu_render: ImGui records into what it is handed
    trace[0] = 0;
    void *cmd = vk_overlay_begin(window);
    if (cmd != window) assert(cb_state[(uintptr_t)cmd - 0x900] == 1);
    vk_overlay_end(window, cmd);
    return trace;
}
int main(int argc, char **argv) {
    (void)argc;
    recorder_on = strcmp(argv[1], "off") != 0;
    if (!strcmp(argv[1], "direct")) setenv("LARECOMP_VK_OVERLAY_DIRECT", "1", 1);
    if (!recorder_on || !strcmp(argv[1], "direct")) {
        for (int f = 0; f < 6; f++) {
            frame_begin();
            assert(!strcmp(overlay(), "drain "));
            frame_end();
        }
        assert(!pools);
        puts("ok");
        return 0;
    }
    // frames 1-3: a pool per slot, no drain, the recorder gets end / begin (secondary contents) / execute
    for (int f = 0; f < 3; f++) {
        frame_begin();
        uint64_t seq = pending;
        assert(!strcmp(overlay(), "create-pool end-secondary end begin-secondary execute "));
        assert(executed == (VkCommandBuffer)(uintptr_t)(0x900 + f) && cur_frame.secondary);
        assert(ov_seq[slot] == seq);
        frame_end();
    }
    // frame 4: slot 1 again, its submission done (vk_frame_begin waited for it): reset, no GPU wait
    completed = 3;
    frame_begin();
    assert(!strcmp(overlay(), "reset-pool end-secondary end begin-secondary execute "));
    // a second overlay in the same frame: inline again, after a drain (the slot's buffer is in this frame's submission)
    assert(!strcmp(overlay(), "end begin-inline drain "));
    assert(!cur_frame.secondary);
    frame_end();
    // frame 5: slot 2, whose submission (2) is done
    frame_begin();
    assert(!strcmp(overlay(), "reset-pool end-secondary end begin-secondary execute "));
    frame_end();
    // frame 6 (slot 0) without an overlay; frame 7: slot 1 again, its submission (4) not done: the GPU wait, then the reset
    frame_begin();
    frame_end();
    frame_begin();
    assert(slot == 1 && ov_seq[1] == 4 && completed == 3);
    assert(!strcmp(overlay(), "gpu-wait reset-pool end-secondary end begin-secondary execute "));
    assert(waited_for == 4);
    frame_end();
    // no frame (minimised window): the window's command buffer path, no secondary handed out
    trace[0] = 0;
    assert(vk_overlay_begin(window) == window && !strcmp(trace, "drain "));
    assert(pools == 3);
    puts("ok");
}
'''
build = (root / 'build').resolve()
build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='vulkan-overlay-', dir=build) as directory:
    temp = Path(directory).resolve()
    assert temp.is_relative_to(build)
    cfile = temp / 'overlay.c'
    cfile.write_text(harness)
    exe = temp / ('overlay.exe' if os.name == 'nt' else 'overlay')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O1', '-Wall', str(cfile), '-o', str(exe)], check=True)
    for mode in ('on', 'off', 'direct'):
        env = {k: v for k, v in os.environ.items() if k != 'LARECOMP_VK_OVERLAY_DIRECT'}
        run = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True, timeout=10)
        assert run.returncode == 0 and run.stdout == 'ok\n', (mode, run)
print('PASS: overlay in a secondary command buffer with the recorder on (no drain), pool reuse after the GPU, '
      'second overlay / recorder off / LARECOMP_VK_OVERLAY_DIRECT=1 into the window buffer after a drain')

// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../rt.h"  // (HEAP_MAX: lazy_heap_predicted)
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../rqueue.h"
#include "vk.h"
#include "record.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../pace.h"
#include "../shader_prep.h"
#include <pthread.h>

VkG g_vk;
#define VK_DEFINE_COMMAND(f) PFN_##f vkp_##f;
#define VK_DEFINE_COMMAND_13(f, ext) PFN_##f vkp_##f;
VK_DEVICE_COMMANDS_12(VK_DEFINE_COMMAND)
VK_DEVICE_COMMANDS_13(VK_DEFINE_COMMAND_13)
#undef VK_DEFINE_COMMAND
#undef VK_DEFINE_COMMAND_13
static SDL_Window *window;

// The system memory heap a whole-heap import (exec.c host_import) lands in and what it can hold: the largest heap with
// a host-visible memory type that isn't device-local (all of them device-local: the largest host-visible one), the
// smaller of its size and its budget (budget 0: unknown). -1: no host-visible heap. Plain types, so
// tools/test_gmem_lowmem.py builds it without Vulkan headers.
typedef struct { uint64_t size, budget; bool device_local, host_visible; } ImportHeap;
static int import_heap_room(const ImportHeap *h, uint32_t n, uint64_t *room) {
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (h[i].host_visible && (pass || !h[i].device_local) && (best < 0 || h[i].size > h[best].size)) best = (int)i;
    if (best >= 0) *room = h[best].budget && h[best].budget < h[best].size ? h[best].budget : h[best].size;
    return best;
}
// What the heap must hold beyond the import: the transient ring's chunks once they spill to system memory (64 MB
// each), readback, the driver's own. Issue #205 (8 GB laptop, Ryzen 5 5500U, AMD's Windows driver): system heap
// 2738 MB, budget 2601 - the 3220 MB import was accepted, then the first submission failed with out of device memory.
// An 8 GB desktop with a GPU of its own (system heap ~3950 MB, budget ~3840) stays on one import.
#define IMPORT_MARGIN (256ull << 20)
// Under this much room one import should fit but might not (a driver's own limits, other programs): the import is
// tried once at the start (exec.c whole_import_probe_oom), with buffer device addresses enabled so lazy chunks can take
// over. Machines with more (an RTX with 16 GB or more of RAM) skip the probe and keep the device as before.
#define IMPORT_PROBE_BELOW (2 * HEAP_MAX)

// Whether one import of the whole heap fits its system memory heap (import_heap_room), from the physical device;
// *probe: it fits, but not by much (IMPORT_PROBE_BELOW), or LARECOMP_VK_SIM_HEAP_OOM=1 (the probe then fails).
static bool whole_import_fits(bool budget_ext, bool *probe) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                             .pNext = budget_ext ? &bp : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp2);
    const VkPhysicalDeviceMemoryProperties *mp = &mp2.memoryProperties;
    ImportHeap h[VK_MAX_MEMORY_HEAPS] = {{0}};
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        h[i].size = mp->memoryHeaps[i].size;
        h[i].budget = budget_ext ? bp.heapBudget[i] : 0;
        h[i].device_local = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    }
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) h[mp->memoryTypes[i].heapIndex].host_visible = true;
    uint64_t room = 0;
    int heap = import_heap_room(h, mp->memoryHeapCount, &room);
    const char *sim = getenv("LARECOMP_VK_SIM_HEAP_OOM");
    if (heap < 0 || room >= HEAP_MAX + IMPORT_MARGIN) {
        *probe = (heap >= 0 && room < IMPORT_PROBE_BELOW) || (sim && atoi(sim));
        return true;
    }
    LOG("[vk] guest heap: memory heap %d holds %llu MB (size %llu, budget %llu), one import needs %llu MB: "
        "imported in chunks as the game binds them\n", heap, (unsigned long long)(room >> 20),
        (unsigned long long)(h[heap].size >> 20), (unsigned long long)(h[heap].budget >> 20),
        (unsigned long long)((HEAP_MAX + IMPORT_MARGIN) >> 20));
    return false;
}

// Whether the guest heap will be imported chunk by chunk (exec.c gmem_lazy): on a GPU whose storage buffers can't
// span the heap (maxStorageBufferRange < HEAP_MAX: Intel, 1 GB); where the system memory heap or its budget can't hold
// one import of the whole heap (whole_import_fits: 8 GB laptops, issue #205); and on RADV when the dma-buf heap
// (exec.c guest_heap_dmabuf) has no cached host-visible memory type that holds it - the same test, which needs only
// the physical device - or LARECOMP_VK_GMEM_LAZY=1 (=0: never; a GPU with a short range then gets windows of one
// import, exec.c gmem_chunk_shift). Decided before the device exists because the lazy import needs buffer device
// addresses, and on RADV enabling them puts every allocation on the driver's global BO list (amdgpu checks all of
// them each submission): a device that imports the heap another way shouldn't pay for that.
// *probe: one import is to be tried first (whole_import_fits), lazy chunks if it runs out of memory.
static bool lazy_heap_predicted(bool budget_ext, bool *probe) {
    *probe = false;
    const char *e = getenv("LARECOMP_VK_GMEM_LAZY");
    if (e) return atoi(e) != 0;
    if (getenv("LARECOMP_VK_NO_GMEM")) return false;
    // Pokémon Z-A has no heap: its window is one import (exec.c gmem_chunk_decide), and this prediction - sized on
    // Link's Awakening's heap - only turned the lazy machinery on beside it: on a Steam Deck (heap 0 holds 2892 MB) the
    // player, statues, trees and banner text went undrawn, complete with LARECOMP_VK_GMEM_LAZY=0 (#50). (Asked of the
    // data root's files: the boot comes after the device)
    extern bool game_files_are_la(const char *game_dir);
    if (!game_files_are_la("extracted")) return false;
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    if (lim && lim < HEAP_MAX) return true;  // (UHD 770 main pass: lazy 8.4 ms, 512 MB windows 8.5)
#ifdef _WIN32
    // The Windows AMD integrated driver accepts one import of the whole heap (the probe writes both ends) and then
    // faults a shader read of it: device lost, as soon as a save loads. Chunks stay inside the pages it maps.
    if (g_vk.props.vendorID == 0x1002 && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        return true;
#endif
    if (!whole_import_fits(budget_ext, probe)) return true;
    if (!strstr(g_vk.props.deviceName, "RADV")) return false;
    *probe = false;  // (RADV: as before - the dma-buf heap or lazy chunks, no probe)
#ifdef __linux__
    const char *d = getenv("LARECOMP_VK_HEAP_DMABUF");
    if ((d ? atoi(d) != 0 : true) && (g_vk.dmabuf || g_vk.map_placed)) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & want) == want && mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= HEAP_MAX) return false;
    }
#endif
    return true;
}
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static void exit_after_shot(void);  // (UI thread section)

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static atomic_uint n;
    bool stop = sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && getenv("LARECOMP_VK_VALIDATION_STOP");
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (stop || atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 500)) LOG("[vk] %s\n", d->pMessage);
    if (stop) {
        LOG("[vk] stopping at the first validation error\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);  // do not run GPU cleanup after the validation failure
    }
    return VK_FALSE;
}

static bool ab_inst_ext, ab_on;  // (GPU address binding report: below, after g_fault_ext)
static void ab_messenger_create(void);
// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
// LARECOMP_VK_VALIDATION_STOP=1: require that layer and its callback, and exit nonzero on the first error.
static bool validating;  // the validation layer is on (validation_exit)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[20];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool strict = getenv("LARECOMP_VK_VALIDATION_STOP") != NULL;
    bool validate = strict || getenv("LARECOMP_VK_VALIDATION") != NULL;
    bool want_ab = !ENV("LARECOMP_VK_ADDR_REPORT") || atoi(ENV("LARECOMP_VK_ADDR_REPORT")) != 0;
    {   // the window's HDR colour spaces (HDR10, scRGB) for F1 > Video > HDR (create_swapchain), where the loader has them;
        // debug utils for the GPU address binding report (ab_*)
        uint32_t na = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &na, NULL);
        VkExtensionProperties *av = calloc(na ? na : 1, sizeof *av);
        vkEnumerateInstanceExtensionProperties(NULL, &na, av);
        for (uint32_t i = 0; i < na; i++) {
            if (!strcmp(av[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
            if (!strcmp(av[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && want_ab) ab_inst_ext = true;
        }
        free(av);
    }
    if (ab_inst_ext && !validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;  // (last: dropped below when the layer is missing)
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        if (strict) {
            LOG("[vk] cannot create an instance with required validation (%d)\n", r);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
        ab_inst_ext = false;  // (debug utils went with the layer)
    }
    validating = validate;
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate && want_ab) ab_inst_ext = true;
    ab_messenger_create();
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        VkResult result = f ? f(g_vk.inst, &mi, NULL, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS && strict) {
            LOG("[vk] required validation callback unavailable (%d)\n", result);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        if (result == VK_SUCCESS) LOG("[vk] validation enabled%s\n", strict ? " (stop on error)" : "");
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first, Vulkan 1.3 before 1.2 (LARECOMP_GPU=<index> picks one); needs Vulkan 1.2 and a queue that can
// present. A Vulkan 1.2 GPU does without the 1.3 features (compat.c).
static bool g_intel_rp;  // (pick_device: this Intel driver runs on render passes)
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = ENV("LARECOMP_GPU") ? atoi(ENV("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_2) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 6 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 4 : 2;
        if (p.apiVersion >= VK_API_VERSION_1_3) score++;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.2 device: the GPU driver is too old, or the GPU has no Vulkan 1.2 driver\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_gpu_vendor = g_vk.props.vendorID;  // (settings.c gpu_feature_on: the Compatibility features' Auto)
    // Intel's Windows drivers before Vulkan 1.3.216 (UHD 630 1.3.215 #8 #46, Iris Xe 1.3.212 #88) crash inside
    // vkCmdBeginRendering reading near a null pointer, and placeholder attachments only moved the crash to a later pass:
    // those run on render passes instead, the compat_rp.c way (LARECOMP_VK_INTEL_RENDER_PASSES=0: dynamic rendering)
    const char *irp = getenv("LARECOMP_VK_INTEL_RENDER_PASSES");
    g_intel_rp = irp ? atoi(irp) != 0 : g_vk.props.vendorID == 0x8086 && g_vk.props.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 216);
    g_vkc.api = g_vk.props.apiVersion >= VK_API_VERSION_1_3 && !ENV("LARECOMP_VK_COMPAT") && !g_intel_rp ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i, g_vk.ts_bits = qp[i].timestampValidBits;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    {   // (with the [system] line, runtime/main.c: kind, video memory, driver - what a report needs)
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        uint64_t vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[i].size;
        VkPhysicalDeviceType t = g_vk.props.deviceType;
        LOG("[vk] using %s (%s, %.1f GB video memory, driver %s %s, Vulkan %u.%u.%u)\n", g_vk.props.deviceName,
            t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "dedicated" : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : "other",
            (double)vram / (1u << 30), dp.driverName, dp.driverInfo, VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
            VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion));
    }
    settings_resolve_defaults(g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    {   // this PC's recommended video settings (settings.c): applied at the first start, offered to older settings files
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        uint64_t vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[i].size;
        settings_video_recommend(g_vk.props.vendorID, g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, vram,
                                 g_vk.props.deviceName);
    }
    // On by default on integrated GPUs only: lean usage and the SMAA stencil measured faster on an Intel UHD 770 and
    // slightly slower on an RTX 3080 Ti; skipping the per-frame target clears saved 3.7 ms on the UHD 770 (2x) with
    // identical images. LARECOMP_VK_LEAN_USAGE / LARECOMP_VK_SMAA_STENCIL / LARECOMP_NO_FRAME_CLEAR =0 / =1 override.
    bool igpu = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    const char *e = getenv("LARECOMP_VK_LEAN_USAGE");
    g_vk.lean_usage = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_VK_SMAA_STENCIL");
    g_vk.smaa_stencil = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_NO_FRAME_CLEAR");
    g_vk.no_frame_clear = e && *e ? atoi(e) != 0 : igpu;
    LOG("[vk] lean usage %s, SMAA stencil %s, per-frame target clears %s (%s GPU)\n", g_vk.lean_usage ? "on" : "off",
        g_vk.smaa_stencil ? "on" : "off", g_vk.no_frame_clear ? "off" : "on", igpu ? "integrated" : "not an integrated");
}

static bool g_fault_ext;

// ------------------------------------------------------------------ GPU address bindings (VK_EXT_device_address_binding_report)
// The AMD battle page faults (#22, #39: RX 9070 XT, RX 7600) read addresses that cluster within each version (v0.1.11:
// 0x19bd81000..0x19bd9c000) - past the end of one big allocation? The driver reports every GPU address range it binds
// and unbinds; at a device fault (vk_device_fault_report) the faulting address is matched against them: the object
// holding it, or the nearest ones on each side, and any freed object that held it (a use after free). The first
// AB_KEEP events stay (the guest window import and other long-lived buffers are made at boot), later ones go to a
// ring. LARECOMP_VK_ADDR_REPORT=0: off.
typedef struct { uint64_t base, size, handle; uint32_t objtype, seq; uint8_t unbind, internal; } AbEvent;
#define AB_KEEP 8192
#define AB_RING (1u << 17)
static AbEvent *ab_ev;  // [AB_KEEP + AB_RING]
static uint32_t ab_n;   // events seen (seq)
static atomic_flag ab_lock = ATOMIC_FLAG_INIT;
static VkDebugUtilsMessengerEXT ab_messenger;

static VKAPI_ATTR VkBool32 VKAPI_CALL addr_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)sev, (void)user;
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !ab_ev) return VK_FALSE;
    const VkBaseInStructure *p = d->pNext;
    while (p && p->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) p = p->pNext;
    if (!p) return VK_FALSE;
    const VkDeviceAddressBindingCallbackDataEXT *b = (const void *)p;
    AbEvent e = {b->baseAddress, b->size, d->objectCount ? d->pObjects[0].objectHandle : 0,
                 d->objectCount ? (uint32_t)d->pObjects[0].objectType : 0, 0,
                 b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT,
                 (b->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0};
    while (atomic_flag_test_and_set_explicit(&ab_lock, memory_order_acquire)) {}
    e.seq = ab_n;
    ab_ev[ab_n < AB_KEEP ? ab_n : AB_KEEP + (ab_n - AB_KEEP) % AB_RING] = e;
    ab_n++;
    atomic_flag_clear_explicit(&ab_lock, memory_order_release);
    return VK_FALSE;
}

static void ab_messenger_create(void) {
    if (!ab_inst_ext) return;
    PFN_vkCreateDebugUtilsMessengerEXT f =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mi = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT, .pfnUserCallback = addr_cb};
    ab_ev = calloc(AB_KEEP + AB_RING, sizeof *ab_ev);
    if (!ab_ev || !f || f(g_vk.inst, &mi, NULL, &ab_messenger) != VK_SUCCESS) { free(ab_ev); ab_ev = NULL; ab_inst_ext = false; }
}

static const AbEvent *ab_at(uint32_t i) {  // i-th kept event, oldest first
    uint32_t kept = ab_n < AB_KEEP ? ab_n : AB_KEEP;
    if (i < kept) return &ab_ev[i];
    uint32_t later = ab_n - AB_KEEP, inring = later < AB_RING ? later : AB_RING;
    return &ab_ev[AB_KEEP + (later - inring + (i - kept)) % AB_RING];
}
static uint32_t ab_count(void) { return ab_n < AB_KEEP ? ab_n : AB_KEEP + (ab_n - AB_KEEP < AB_RING ? ab_n - AB_KEEP : AB_RING); }

// Which kept bind events were unbound later (freed / rebound elsewhere): one pass from the newest event back, the
// unbinds seen so far in a hash set of (object, address).
static uint8_t *ab_freed_flags(uint32_t n) {
    enum { HB = 19 };
    uint8_t *freed = calloc(n ? n : 1, 1);
    uint64_t *set = calloc(1u << HB, sizeof *set);
    if (!freed || !set) { free(set); return freed; }
    for (uint32_t j = n; j-- > 0;) {
        const AbEvent *e = ab_at(j);
        uint64_t k = (e->handle * 0x9E3779B97F4A7C15ull ^ e->base) | 1;
        uint32_t h = (uint32_t)(k >> 40) & ((1u << HB) - 1);
        while (set[h] && set[h] != k) h = (h + 1) & ((1u << HB) - 1);
        if (e->unbind) set[h] = k;  // (at most AB_KEEP + AB_RING keys: the table stays under half full)
        else freed[j] = set[h] == k;
    }
    free(set);
    return freed;
}

static void ab_describe(const char *what, uint32_t i, bool freed, uint64_t addr) {
    extern const char *vk_exec_buffer_kind(uint64_t handle);
    const AbEvent *e = ab_at(i);
    const char *kind = e->objtype == VK_OBJECT_TYPE_BUFFER || e->objtype == VK_OBJECT_TYPE_DEVICE_MEMORY ? vk_exec_buffer_kind(e->handle) : NULL;
    const char *type = e->objtype == VK_OBJECT_TYPE_BUFFER ? "buffer" : e->objtype == VK_OBJECT_TYPE_IMAGE ? "image"
                     : e->objtype == VK_OBJECT_TYPE_DEVICE_MEMORY ? "memory" : "object";
    uint64_t end = e->base + e->size;
    const char *where = addr >= end ? "the fault past its end by" : addr < e->base ? "the fault before its start by" : "the fault at offset";
    uint64_t rel = addr >= end ? addr - end : addr < e->base ? e->base - addr : addr - e->base;
    LOG("[vk]   %s: %s %#llx%s%s%s [%#llx, %#llx) %llu KB, %s %llu KB, bound at event %u of %u%s%s\n", what, type,
        (unsigned long long)e->handle, kind ? " (" : "", kind ? kind : "", kind ? ")" : "", (unsigned long long)e->base,
        (unsigned long long)end, (unsigned long long)(e->size >> 10), where, (unsigned long long)(rel >> 10), e->seq, ab_n,
        e->internal ? ", driver-internal" : "", freed ? ", FREED since" : "");
}

// The guest window's import (exec.c gmem_import, with LARECOMP_VK_GMEM_ADDR): its device address range and the guest
// address it starts at. A fault inside it is named as the guest page, with that page's state on the host now: the
// address binding report doesn't list imported host memory, so these faults read as "no object ever held that page".
uint64_t g_gmem_va, g_gmem_guest, g_gmem_va_size;
static void gmem_fault_explain(uint64_t va) {
    if (!g_gmem_va || va < g_gmem_va || va >= g_gmem_va + g_gmem_va_size) {
        if (g_gmem_va) LOG("[vk]   (outside the guest window import [%#llx, %#llx))\n", (unsigned long long)g_gmem_va,
                           (unsigned long long)(g_gmem_va + g_gmem_va_size));
        return;
    }
    uint64_t ga = g_gmem_guest + (va - g_gmem_va);
    extern int nvn_pool_flags_at(uint64_t addr);
    LOG("[vk]   inside the guest window import: guest %llx (+%llu MB), memory pool flags %#x\n", (unsigned long long)ga,
        (unsigned long long)((va - g_gmem_va) >> 20), nvn_pool_flags_at(ga));
    char st[160];
    if (plat_page_state(ga, st, sizeof st)) LOG("[vk]   that page now: %s\n", st);
}

// At a device fault: what held the faulting page [addr, addr + precision), and the live objects nearest to it
static void ab_explain(uint64_t addr, uint64_t prec) {
    if (!ab_on) { LOG("[vk]   address owner: unknown (%s)\n", ab_inst_ext ? "the driver has no address binding report" :
                                                                  "no debug utils, or LARECOMP_VK_ADDR_REPORT=0"); return; }
    while (atomic_flag_test_and_set_explicit(&ab_lock, memory_order_acquire)) {}
    uint32_t n = ab_count();
    uint8_t *freed = ab_freed_flags(n);
    uint64_t lo = addr, hi = addr + (prec ? prec : 1);
    int shown = 0, below = -1, above = -1;
    for (uint32_t i = 0; i < n; i++) {
        const AbEvent *e = ab_at(i);
        if (e->unbind || !e->size) continue;
        bool f = freed && freed[i];
        if (e->base < hi && e->base + e->size > lo) { if (shown++ < 8) ab_describe("held by", i, f, addr); continue; }
        if (f) continue;  // (neighbours: live ones only)
        if (e->base + e->size <= lo && (below < 0 || e->base + e->size > ab_at((uint32_t)below)->base + ab_at((uint32_t)below)->size)) below = (int)i;
        if (e->base >= hi && (above < 0 || e->base < ab_at((uint32_t)above)->base)) above = (int)i;
    }
    if (!shown) LOG("[vk]   no object ever held that page in the %u kept binding events (%u in all)\n", n, ab_n);
    // (the nearest edges: every live object ending / starting there - a buffer and its memory share one)
    uint64_t bend = below >= 0 ? ab_at((uint32_t)below)->base + ab_at((uint32_t)below)->size : 0;
    uint64_t abase = above >= 0 ? ab_at((uint32_t)above)->base : 0;
    int nb = 0, na = 0;
    for (uint32_t i = 0; i < n; i++) {
        const AbEvent *e = ab_at(i);
        if (e->unbind || !e->size || (freed && freed[i])) continue;
        if (below >= 0 && e->base + e->size == bend && nb++ < 4) ab_describe("nearest below", i, false, addr);
        if (above >= 0 && e->base == abase && na++ < 4) ab_describe("nearest above", i, false, addr);
    }
    free(freed);
    atomic_flag_clear_explicit(&ab_lock, memory_order_release);
}

static PFN_vkWaitForPresentKHR g_present_wait;  // (VK_KHR_present_wait: present_watch)
static bool g_present_fifo;  // (present_watch gave up on the default present mode)
static _Atomic uint32_t g_img_changes;  // (probe_check: the game's presented image changed - screen_watch)
static _Atomic bool g_screen_stale;     // (screen_watch -> vk_frame_end: the window stopped showing the frames)
static _Atomic bool g_present_unseen;   // (vk_present_watch, on the recorder -> vk_frame_end: the same, and why)
static char g_present_unseen_why[96];
static bool g_ckpt;
static PFN_vkCmdSetCheckpointNV p_set_ckpt;

void vk_checkpoint(VkCommandBuffer cb, uint64_t mark) {
    if (!g_ckpt) return;
    if (!p_set_ckpt) p_set_ckpt = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetCheckpointNV");
    if (p_set_ckpt) p_set_ckpt(cb, (void *)(uintptr_t)mark);
}

uint32_t vk_checkpoints(uint64_t *mark, uint32_t *stage, uint32_t max) {
    if (!g_ckpt) return 0;
    PFN_vkGetQueueCheckpointDataNV get = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(g_vk.dev, "vkGetQueueCheckpointDataNV");
    if (!get) return 0;
    uint32_t n = 0;
    get(g_vk.queue, &n, NULL);
    if (n > max) n = max;
    VkCheckpointDataNV d[16];
    for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    get(g_vk.queue, &n, d);
    for (uint32_t i = 0; i < n; i++) { mark[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker; stage[i] = d[i].stage; }
    return n;
}
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
// The GPU met an invalid instruction ("ip invalid", #93: the same move on one PC, every time, with or without mods): the
// code it runs comes from the driver, not from us - GLSL can't express one. Two things can give it, and neither needs the
// player: code kept in the pipeline cache, which goes now (and isn't saved over: the next start builds it again), and
// then, if it happens again with a new cache on the same driver, the driver's optimizer: cache/gpu_bad_code counts them
// per driver, and from the second on pipelines are built without optimization (g_vk_no_opt) until the driver changes.
// The driver keeps a disk cache of its own, out of our reach, and Vulkan device losses on 617.42 / 621.01 have been tied to
// pipelines coming from it (NVIDIA forum, RTX 5070 Ti): the count also goes in each shader's SPIR-V header (g_vk_spirv_salt:
// the generator word, which changes nothing but the shader's hash), so after a fault the driver builds everything anew.
// LARECOMP_VK_BAD_CODE_FIX=0: off. LARECOMP_TEST_BAD_CODE=<frame>: this path at that frame, then a stop.
#define BAD_CODE_FILE "cache/gpu_bad_code"
#define PCACHE_FILE "cache/vk_pipelines.bin"
#define PCACHE_TAG "cache/vk_pipelines.bin.tag"
#define PCACHE_MAX (512ull << 20)
bool g_vk_no_opt;
uint32_t g_vk_spirv_salt;
static bool pcache_off;
static int bad_code_count(void) {  // this driver's
    unsigned drv = 0;
    int n = 0;
    FILE *f = fopen(BAD_CODE_FILE, "r");
    if (f) {
        if (fscanf(f, "%u %d", &drv, &n) != 2) n = 0;
        fclose(f);
    }
    return drv == g_vk.props.driverVersion ? n : 0;
}
void vk_bad_code_note(void) {
    static bool done;
    const char *e = getenv("LARECOMP_VK_BAD_CODE_FIX");
    if (done || (e && !atoi(e))) return;
    done = true;
    pcache_off = true;
    remove(PCACHE_FILE);
    remove(PCACHE_TAG);
    int n = bad_code_count() + 1;
    FILE *f = fopen(BAD_CODE_FILE, "w");
    if (f) { fprintf(f, "%u %d\n", g_vk.props.driverVersion, n); fclose(f); }
    rt_gpu_bad_code = n;
    LOG("[vk] the GPU met an invalid instruction (%d on this driver): pipeline cache deleted%s\n", n,
        n >= 2 ? ", the next start builds pipelines without optimization" : "");
}

// The driver's shader compiler itself can crash on one pipeline (#93, RTX 5060 Ti, 621.01: inside nvgpucomp64.dll, in
// vkCreateGraphicsPipelines): its two shaders go in cache/gpu_bad_shaders, by driver, and from the next start that pipeline
// alone is built without optimization. Graphics pipelines only (t_build_*: this thread's build).
#define BAD_SHADERS_FILE "cache/gpu_bad_shaders"
__thread uint64_t t_build_vs, t_build_fs;
static struct { uint64_t vs, fs; } bad_sh[64];
static int nbad_sh;
static void bad_shaders_load(void) {
    FILE *f = fopen(BAD_SHADERS_FILE, "r");
    if (!f) return;
    unsigned drv;
    unsigned long long v, s;
    while (nbad_sh < 64 && fscanf(f, "%u %llx %llx", &drv, &v, &s) == 3)
        if (drv == g_vk.props.driverVersion) bad_sh[nbad_sh].vs = v, bad_sh[nbad_sh++].fs = s;
    fclose(f);
    if (nbad_sh) LOG("[vk] %d pipeline(s) built without optimization: this driver's compiler crashed on them\n", nbad_sh);
}
bool vk_shader_bad(uint64_t vs, uint64_t fs) {
    for (int i = 0; i < nbad_sh; i++) if (bad_sh[i].vs == vs && bad_sh[i].fs == fs) return true;
    return false;
}
// (crash_explain.c, on the crashing thread, for a crash in the driver's code)
void vk_compile_crash_note(void) {
    if (!t_build_vs && !t_build_fs) return;
    bool again = vk_shader_bad(t_build_vs, t_build_fs);
    LOG("[vk] the driver's compiler crashed on pipeline vs %016llx fs %016llx%s\n", (unsigned long long)t_build_vs,
        (unsigned long long)t_build_fs, again ? ", already built without optimization" : "");
    rt_gpu_bad_code = again ? -2 : -1;
    if (again) return;
    FILE *f = fopen(BAD_SHADERS_FILE, "a");
    if (f) { fprintf(f, "%u %llx %llx\n", g_vk.props.driverVersion, (unsigned long long)t_build_vs, (unsigned long long)t_build_fs); fclose(f); }
}

void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
    {
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
        if (addr[i].addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT) vk_bad_code_note();
        gmem_fault_explain(addr[i].reportedAddress);
        if (addr[i].addressType >= VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT &&
            addr[i].addressType <= VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT)
            ab_explain(addr[i].reportedAddress & ~(addr[i].addressPrecision ? addr[i].addressPrecision - 1 : 0), addr[i].addressPrecision);
    }
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
    // the switches the AMD faults have been bisected with (#22, #39, #54): the next report names its own state
    // (as they ran: F1 > Compatibility, the LARECOMP_* variables and the per-GPU defaults all applied)
    extern bool vk_overlap_state(void), vk_xfb_mirror_state(void), maxwell_flat_state(void);
    const char *rt = getenv("LARECOMP_RENDER_THREAD");
    LOG("[vk]   state: straight-line shaders %s, xfb mirror %s, render thread %s, dynamic vertex input %s, GPU overlap %s"
        " (F1 > Rendering > Compatibility)\n", maxwell_flat_state() ? "on" : "off", vk_xfb_mirror_state() ? "on" : "off",
        rt ? rt : "on (default)", g_vk.vids ? "on" : "off", vk_overlap_state() ? "on" : "off");
}

// Each memory heap: its size, the driver's budget for this process (smaller when other programs hold that memory),
// this process's use and the part LAS Recomp allocated through VMA. Logged at start and when an allocation fails
// (issue #45: out of device memory on a 6 GB GTX 1060 - our own use, or other programs'?).
void vk_log_memory(const char *why) {
    if (!g_vk.vma) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        LOG("[vk] memory (%s) heap %u %s: %llu MB, budget %llu MB, in use %llu MB, ours %llu MB in %u allocations\n", why, i,
            mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "system",
            (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(b[i].budget >> 20),
            (unsigned long long)(b[i].usage >> 20), (unsigned long long)(b[i].statistics.allocationBytes >> 20),
            b[i].statistics.allocationCount);
}

// ---- pipeline cache file
static size_t pcache_saved;  // bytes of the last save (or load)
void vk_pipeline_cache_save(void);

static void pipeline_cache_tag(char *out, size_t n) {
    char gpu[400], uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(gpu, sizeof gpu, "%s, vendor %x device %x, driver %u, cache %s", g_vk.props.deviceName, g_vk.props.vendorID,
             g_vk.props.deviceID, g_vk.props.driverVersion, uuid);
    shader_cache_tag(out, n, gpu);
}

static void *pipeline_cache_saver(void *a) {  // every 3 minutes, when it grew
    (void)a;
    for (;;) {
        SDL_Delay(180000);
        if (rq_stopping()) break;  // (quitting: backend_quit saves it)
        vk_pipeline_cache_save();
    }
    return NULL;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[32];
    uint32_t n_ext = 0;
    // Vulkan 1.2: the 1.3 features the driver has as extensions. LARECOMP_VK_COMPAT=1 takes none of them (the compat.c
    // ways), =<names> only leaves out those: rendering, sync, eds, eds2 (e.g. =sync)
    const char *cm = ENV("LARECOMP_VK_COMPAT");
    if (!cm && g_intel_rp) cm = "rendering";  // (only that: the other 1.3 features stay, as extensions)
#define VK_OFF(name) (cm && (!strcmp(cm, "1") || strstr(cm, name)))
    bool v13 = g_vkc.api >= VK_API_VERSION_1_3;
    bool x_dr = !v13 && !VK_OFF("rendering") && has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    bool x_s2 = !v13 && !VK_OFF("sync") && has_ext(ep, ne, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    bool x_ds = !v13 && !(VK_OFF("eds") && !strstr(cm, "eds2")) && has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    bool x_ds2 = !v13 && !VK_OFF("eds2") && has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
#undef VK_OFF
    if (x_dr) ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    if (x_s2) ext[n_ext++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    if (x_ds) ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
    if (x_ds2) ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    VkPhysicalDeviceAddressBindingReportFeaturesEXT ab_feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT};
    if (ab_inst_ext && has_ext(ep, ne, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME)) {  // (GPU address owners: ab_explain)
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ab_feat};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        ab_on = ab_feat.reportAddressBinding;
        ab_feat.pNext = NULL;
        if (ab_on) ext[n_ext++] = VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME;
    }
    LOG("[vk] GPU address binding report: %s\n", ab_on ? "on (a device fault names the object at its address)"
        : !ab_inst_ext ? "off (no debug utils, or LARECOMP_VK_ADDR_REPORT=0)" : "not offered by this driver");
    // (LARECOMP_VK_DEVLOST_TRACE: the last draw / dispatch reached, from the driver)
    g_ckpt = ENV("LARECOMP_VK_DEVLOST_TRACE") && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (g_ckpt) ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    bool budget = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (real per-heap budgets: vk_log_memory)
    if (budget) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // pipeline-building features: logged below (issue #92); dynamic vertex input used, the library one only measured
    bool vids_ext = has_ext(ep, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
#ifdef VK_EXT_graphics_pipeline_library
    bool gpl_ext = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                   has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
#endif
#ifdef VK_EXT_extended_dynamic_state3
    bool eds3_ext = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
#endif
#ifdef __linux__
    // the guest heap as driver memory mapped at HEAP_BASE (exec.c backend_guest_heap_prepare)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    // ... mapped there by the driver itself (its own DRM file, as every vkMapMemory), not through the dma-buf (issue #92)
    // (older Vulkan headers, e.g. a distro's, lack it: then the dma-buf mapping only)
#ifdef VK_EXT_map_memory_placed
    bool placed_ext = has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
#endif
#endif
    // Shader reads past the buffer / image they were given: zeros instead of a GPU page fault (#22: an RX 9070 XT lost
    // the device on an invalid read in a battle, where NVIDIA reads on). Off on AMD by default since: on driver
    // 26.10.1 the same card loses the device with it, entering the world ("read invalid 0xbeefdeadc000", #39, #54 -
    // isolated: alone of the features tried, it crashed when turned back on) and v0.1.9 in a battle. F1 >
    // Compatibility; LARECOMP_VK_ROBUST2=0/1.
    bool x_rb2 = gpu_feature_on(GPU_ROBUST2, "LARECOMP_VK_ROBUST2") && has_ext(ep, ne, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    // Whether the window really shows the frames presented (present_watch). LARECOMP_NO_PRESENT_WATCH=1: not checked
    bool x_pw = !ENV("LARECOMP_NO_PRESENT_WATCH") && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceRobustness2FeaturesEXT have_rb2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDevicePresentWaitFeaturesKHR have_pw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                      .pNext = x_rb2 ? &have_rb2 : NULL};
    VkPhysicalDevicePresentIdFeaturesKHR have_pid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &have_pw};
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .pNext = x_pw ? (void *)&have_pid : x_rb2 ? (void *)&have_rb2 : NULL};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT have_placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed_ext) have_placed.pNext = have12.pNext, have12.pNext = &have_placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT have_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (vids_ext) have_vids.pNext = have.pNext, have.pNext = &have_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (gpl_ext) have_gpl.pNext = have.pNext, have.pNext = &have_gpl;
#endif
#ifdef VK_EXT_extended_dynamic_state3
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT have_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (eds3_ext) have_eds3.pNext = have.pNext, have.pNext = &have_eds3;
#endif
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    x_rb2 = x_rb2 && have.features.robustBufferAccess && have_rb2.robustBufferAccess2 && have_rb2.robustImageAccess2;
    if (x_rb2) ext[n_ext++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    x_pw = x_pw && have_pid.presentId && have_pw.presentWait;
    if (x_pw) ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME, ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    if (!have12.shaderOutputLayer) LOG("[vk] warning: no shaderOutputLayer (vertex shaders choosing the render target layer)\n");
    {   // what the driver offers for building pipelines (one line, for #92 reports)
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        char gpl[96] = "no extension", eds3[160] = "no extension";
#ifdef VK_EXT_graphics_pipeline_library
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (gpl_ext) gplp.pNext = pr.pNext, pr.pNext = &gplp;
#else
        snprintf(gpl, sizeof gpl, "not in this build's headers");
#endif
#ifndef VK_EXT_extended_dynamic_state3
        snprintf(eds3, sizeof eds3, "not in this build's headers");
#endif
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pr);
#ifdef VK_EXT_graphics_pipeline_library
        if (gpl_ext)
            snprintf(gpl, sizeof gpl, "%s (fast linking %s, independent interpolation decoration %s)",
                     have_gpl.graphicsPipelineLibrary ? "yes" : "no", gplp.graphicsPipelineLibraryFastLinking ? "yes" : "no",
                     gplp.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes" : "no");
#endif
#ifdef VK_EXT_extended_dynamic_state3
        if (eds3_ext)
            snprintf(eds3, sizeof eds3, "polygon mode %d, samples %d, alpha to coverage %d, blend enable %d, "
                     "blend equation %d, write mask %d", have_eds3.extendedDynamicState3PolygonMode,
                     have_eds3.extendedDynamicState3RasterizationSamples, have_eds3.extendedDynamicState3AlphaToCoverageEnable,
                     have_eds3.extendedDynamicState3ColorBlendEnable, have_eds3.extendedDynamicState3ColorBlendEquation,
                     have_eds3.extendedDynamicState3ColorWriteMask);
#endif
        LOG("[vk] driver %s %s; graphics pipeline library: %s; vertex input dynamic state: %s; extended dynamic state 3: %s\n",
            drv.driverName, drv.driverInfo, gpl, vids_ext ? (have_vids.vertexInputDynamicState ? "yes" : "no") : "no extension",
            eds3);
    }
    {   // dynamic vertex input (exec.c): fewer pipelines; LARECOMP_VK_DYN_VERTEX=0 keeps the layout in each pipeline
        const char *e = getenv("LARECOMP_VK_DYN_VERTEX");
        bool off = e && *e && !atoi(e);
        // Intel's frozen Windows driver for its older integrated GPUs (UHD 630, Ice Lake...: Vulkan 1.3.215 and older)
        // draws nothing with it - a black picture (#8, #357); LARECOMP_VK_DYN_VERTEX=1 still turns it on there
        bool intel_legacy = false;
#ifdef _WIN32
        intel_legacy = g_vk.props.vendorID == 0x8086 && g_vk.props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
#endif
        if (intel_legacy && !(e && *e)) off = true;
        // AMD GPUs (RX 7600, RX 9070 XT on 26.10.1) lose the device on v0.1.10 at the first frames in the world, "read
        // invalid 0xbeefdeadc000" (#39, #54), where v0.1.9 - no dynamic vertex input yet - played. Off there by default,
        // as the GPU overlap (exec.c overlap_on); LARECOMP_VK_DYN_VERTEX=1 turns it on
        bool amd = g_vk.props.vendorID == 0x1002;
        if (amd && !(e && *e)) off = true;
        int cs = g_settings.gpu_compat[GPU_DYN_VERTEX];  // (F1 > Compatibility: Auto keeps the rules above)
        if (!(e && *e) && cs != GPU_AUTO) off = cs == GPU_OFF;
        g_vk.vids = vids_ext && have_vids.vertexInputDynamicState && !off;
        if (g_vk.vids) ext[n_ext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
        else LOG("[vk] vertex input: in each pipeline (%s)\n",
                 !(e && *e) && cs == GPU_OFF ? "F1 > Compatibility" :
                 intel_legacy && !(e && *e) ? "Intel's older Windows driver: drew nothing with it" :
                 amd && !(e && *e) ? "off on AMD GPUs, #39; F1 > Compatibility or LARECOMP_VK_DYN_VERTEX=1: on" :
                 off ? "LARECOMP_VK_DYN_VERTEX=0" :
                 !vids_ext ? "no VK_EXT_vertex_input_dynamic_state" : "vertexInputDynamicState not supported");
    }
#ifdef VK_EXT_graphics_pipeline_library
    {   // LARECOMP_VK_GPL_PROBE=1: library builds timed after shader preparation (exec.c gpl_probe); nothing else uses it
        const char *e = getenv("LARECOMP_VK_GPL_PROBE");
        if (e && atoi(e)) {
            g_vk.gpl_probe = gpl_ext && have_gpl.graphicsPipelineLibrary;
            if (g_vk.gpl_probe) {
                ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
                ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
            } else LOG("[gpl] probe: the driver has no graphics pipeline library; skipped\n");
        }
    }
#else
    if (getenv("LARECOMP_VK_GPL_PROBE") && atoi(getenv("LARECOMP_VK_GPL_PROBE"))) LOG("[gpl] probe: this build's Vulkan headers have no graphics pipeline library; skipped\n");
#endif
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    have12.pNext = NULL;
    g_vk.map_placed = placed_ext && have_placed.memoryMapPlaced;
    g_vk.unmap_reserve = g_vk.map_placed && have_placed.memoryUnmapReserve;
    if (g_vk.map_placed) {
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    }
#endif
    bool probe;
    g_vk.lazy_heap = lazy_heap_predicted(budget, &probe);
    g_vk.heap_probe = !g_vk.lazy_heap && probe && g_vk.host_import && have12.bufferDeviceAddress;
    g_vk.bda = (g_vk.lazy_heap || g_vk.heap_probe) && have12.bufferDeviceAddress;
    // (#39 #72: AMD's page faults land where no object the address binding report knows of lies - most likely inside
    // the guest window's import, which that report doesn't list. Its device address, logged at the import, settles
    // that; on by default on AMD, LARECOMP_VK_GMEM_ADDR=0 off / =1 on any GPU)
    const char *ga = getenv("LARECOMP_VK_GMEM_ADDR");
    g_vk.gmem_addr = (ga ? atoi(ga) != 0 : g_vk.props.vendorID == 0x1002) && have12.bufferDeviceAddress && g_vk.host_import;
    g_vk.bda |= g_vk.gmem_addr;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    void *tail = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL;
    VkPhysicalDeviceRobustness2FeaturesEXT rb2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                                                  .robustBufferAccess2 = VK_TRUE, .robustImageAccess2 = VK_TRUE, .pNext = tail};
    if (x_rb2) tail = &rb2;
    VkPhysicalDevicePresentIdFeaturesKHR pid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .presentId = VK_TRUE,
                                                .pNext = tail};
    VkPhysicalDevicePresentWaitFeaturesKHR pw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                 .presentWait = VK_TRUE, .pNext = &pid};
    if (x_pw) tail = &pw;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE, .subgroupSizeControl = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE, .pNext = tail};
    // (Vulkan 1.2: the extensions' feature structures instead)
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT fds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .extendedDynamicState2 = VK_TRUE, .pNext = tail};
    if (x_ds2) tail = &fds2;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT fds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .extendedDynamicState = VK_TRUE, .pNext = tail};
    if (x_ds) tail = &fds;
    VkPhysicalDeviceSynchronization2FeaturesKHR fs2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
                                                       .synchronization2 = VK_TRUE, .pNext = tail};
    if (x_s2) tail = &fs2;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                       .dynamicRendering = VK_TRUE, .pNext = tail};
    if (x_dr) tail = &fdr;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = g_vk.bda, .shaderOutputLayer = have12.shaderOutputLayer,
                                            .samplerFilterMinmax = have12.samplerFilterMinmax,
                                            .shaderOutputViewportIndex = have12.shaderOutputViewportIndex, .pNext = v13 ? (void *)&f13 : tail};
    if (!v13) {  // (an older driver may lack some: say which, and ask only for what it has)
#define VK_WANT12(x) if (f12.x && !have12.x) { LOG("[vk] warning: the GPU driver has no " #x "\n"); f12.x = VK_FALSE; }
        VK_WANT12(timelineSemaphore) VK_WANT12(shaderInt8) VK_WANT12(samplerMirrorClampToEdge)
        VK_WANT12(uniformAndStorageBuffer8BitAccess) VK_WANT12(storageBuffer8BitAccess) VK_WANT12(scalarBlockLayout)
#undef VK_WANT12
    }
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                         .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = g_vk.unmap_reserve,
                                                         .pNext = f.pNext};
    if (g_vk.map_placed) f.pNext = &placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT en_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE};
    if (g_vk.vids) en_vids.pNext = f.pNext, f.pNext = &en_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT en_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    if (g_vk.gpl_probe) en_gpl.pNext = f.pNext, f.pNext = &en_gpl;
#endif
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    g_vk.ssbo_chunks = h->fragmentStoresAndAtomics && h->vertexPipelineStoresAndAtomics && h->robustBufferAccess;
    b->shaderClipDistance = h->shaderClipDistance;
    b->geometryShader = h->geometryShader;  // (fragment shaders reading gl_Layer: the Geometry capability)
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    if (ab_on) ab_feat.pNext = (void *)di.pNext, di.pNext = &ab_feat;
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
#define VK_LOAD_COMMAND(f)                                                                                            \
    if (!(vkp_##f = (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f)))                                                     \
        FATAL("the GPU driver has no %s: update it (Vulkan 1.2 is needed)\n", #f);
    VK_DEVICE_COMMANDS_12(VK_LOAD_COMMAND)
#undef VK_LOAD_COMMAND
    // the 1.3 ones: core, or by the extension's name when it was enabled above (NULL otherwise)
#define VK_LOAD_COMMAND_13(f, ext)                                                                                    \
    vkp_##f = v13 ? (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f) : (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f #ext);
    VK_DEVICE_COMMANDS_13(VK_LOAD_COMMAND_13)
#undef VK_LOAD_COMMAND_13
    if (!v13) {  // (an extension's command is only usable if that extension is enabled)
        if (!x_dr) vkp_vkCmdBeginRendering = NULL, vkp_vkCmdEndRendering = NULL;
        if (!x_s2) vkp_vkCmdPipelineBarrier2 = NULL, vkp_vkQueueSubmit2 = NULL, vkp_vkCmdWriteTimestamp2 = NULL;
        if (!x_ds)
            vkp_vkCmdSetCullMode = NULL, vkp_vkCmdSetFrontFace = NULL, vkp_vkCmdSetPrimitiveTopology = NULL,
            vkp_vkCmdSetDepthTestEnable = NULL, vkp_vkCmdSetDepthWriteEnable = NULL, vkp_vkCmdSetDepthCompareOp = NULL,
            vkp_vkCmdSetStencilTestEnable = NULL, vkp_vkCmdSetStencilOp = NULL;
        if (!x_ds2) vkp_vkCmdSetDepthBiasEnable = NULL, vkp_vkCmdSetPrimitiveRestartEnable = NULL;
    }
    g_vkc.sync1 = !vkp_vkCmdPipelineBarrier2 || !vkp_vkQueueSubmit2 || !vkp_vkCmdWriteTimestamp2;
    g_vkc.render_pass = !vkp_vkCmdBeginRendering || !vkp_vkCmdEndRendering;
    g_vkc.no_eds1 = !vkp_vkCmdSetCullMode || !vkp_vkCmdSetFrontFace || !vkp_vkCmdSetPrimitiveTopology ||
                    !vkp_vkCmdSetDepthTestEnable || !vkp_vkCmdSetDepthWriteEnable || !vkp_vkCmdSetDepthCompareOp ||
                    !vkp_vkCmdSetStencilTestEnable || !vkp_vkCmdSetStencilOp;
    if (g_vkc.no_eds1)
        vkp_vkCmdSetCullMode = NULL, vkp_vkCmdSetFrontFace = NULL, vkp_vkCmdSetPrimitiveTopology = NULL,
        vkp_vkCmdSetDepthTestEnable = NULL, vkp_vkCmdSetDepthWriteEnable = NULL, vkp_vkCmdSetDepthCompareOp = NULL,
        vkp_vkCmdSetStencilTestEnable = NULL, vkp_vkCmdSetStencilOp = NULL;
    g_vkc.no_eds2 = !vkp_vkCmdSetDepthBiasEnable || !vkp_vkCmdSetPrimitiveRestartEnable;
    if (g_vkc.no_eds2) vkp_vkCmdSetDepthBiasEnable = NULL, vkp_vkCmdSetPrimitiveRestartEnable = NULL;
    LOG("[vk] Vulkan %u.%u%s: dynamic rendering %s, synchronization2 %s, extended dynamic state %s / %s\n",
        VK_API_VERSION_MAJOR(g_vkc.api), VK_API_VERSION_MINOR(g_vkc.api), ENV("LARECOMP_VK_COMPAT") ? " (LARECOMP_VK_COMPAT)" : "",
        g_vkc.render_pass ? "no (render passes)" : "yes", g_vkc.sync1 ? "no (Vulkan 1.0 barriers)" : "yes",
        g_vkc.no_eds1 ? "no (in the pipelines)" : "yes", g_vkc.no_eds2 ? "no (in the pipelines)" : "yes");
    if (g_vk.vids) {
        g_vk.set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetVertexInputEXT");
        if (!g_vk.set_vertex_input) g_vk.vids = false;
        LOG("[vk] vertex input: %s\n", g_vk.vids ? "dynamic (VK_EXT_vertex_input_dynamic_state)" :
            "in each pipeline (vkCmdSetVertexInputEXT missing)");
    }
    LOG("[vk] robust buffer / image access 2: %s\n", x_rb2 ? "yes" : "no");
    g_present_wait = x_pw ? (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR") : NULL;
    LOG("[vk] present watch: %s\n", g_present_wait ? "yes" : "no (no VK_KHR_present_wait)");

    // (VMA's commands from the driver too, as above)
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .pVulkanFunctions = &vf, .vulkanApiVersion = g_vkc.api,
                                 .flags = budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_log_memory("start");

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save), used only with its tag
    // (vk_pipelines.bin.tag: this build, the translator, the GPU and its driver - another one starts it empty)
    char tag[512];
    pipeline_cache_tag(tag, sizeof tag);
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = shader_cache_tag_ok(PCACHE_TAG, tag) ? fopen(PCACHE_FILE, "rb") : NULL;
    if (!fp && !access(PCACHE_FILE, F_OK)) LOG("[vk] pipeline cache from another build / GPU / driver: starting a new one\n");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        // (it only ever grows - 703 MB on #93's PC, ours about 200 after long sessions: past this, a new one)
        if (sz > PCACHE_MAX) {
            LOG("[vk] pipeline cache: %zu MB is too much: starting a new one\n", sz >> 20);
            sz = 0;
        }
        if (sz) {
            data = malloc(sz);
            if (fread(data, 1, sz, fp) != sz) sz = 0;
        }
        fclose(fp);
    }
    const char *bad_e = getenv("LARECOMP_VK_BAD_CODE_FIX");
    g_vk_no_opt = !(bad_e && !atoi(bad_e)) && bad_code_count() >= 2;
    g_vk_spirv_salt = bad_e && !atoi(bad_e) ? 0 : (uint32_t)bad_code_count();
    if (g_vk_spirv_salt) LOG("[vk] shaders salted (%u): the driver's own cache isn't used for them\n", g_vk_spirv_salt);
    if (g_vk_no_opt) LOG("[vk] pipelines without optimization: the GPU met invalid instructions twice on this driver\n");
    bad_shaders_load();
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
    if (pc.initialDataSize) LOG("[vk] pipeline cache: loaded %zu bytes\n", pc.initialDataSize);
    pcache_saved = pc.initialDataSize;
    pthread_t t;  // (kept every few minutes while it grows, not only at a clean exit)
    pthread_create(&t, NULL, pipeline_cache_saver, NULL);
    pthread_detach(t);
}

// Saved atomically (temp file + rename: a crash mid-write leaves the previous one), with its tag; one save at a time
void vk_pipeline_cache_save(void) {
    if (pcache_off) return;  // (vk_bad_code_note: what it holds is what went wrong)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && sz != pcache_saved) {
        void *data = malloc(sz);
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS && shader_cache_write(PCACHE_FILE, data, sz)) {
            char tag[512];
            pipeline_cache_tag(tag, sizeof tag);
            if (!shader_cache_tag_ok(PCACHE_TAG, tag)) shader_cache_tag_write(PCACHE_TAG, tag);
            pcache_saved = sz;
            LOG("[vk] pipeline cache: saved %zu bytes\n", sz);
        }
        free(data);
    }
    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static bool present_fifo_made;  // (the Frame pacing setting the swapchain was made with: vk_frame_begin)
static VkPresentModeKHR present_mode(void) {
    present_fifo_made = g_settings.present_fifo;
    if (g_present_fifo) return VK_PRESENT_MODE_FIFO_KHR;  // (present_watch: the display stopped taking the frames twice)
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    // F1 > Video > Frame pacing Smooth: FIFO. With MAILBOX the game's own 60 Hz clock (vi.c) and the display's drift
    // apart, and while they cross, frames are dropped and shown twice for a few seconds at a steady 60 fps (judder
    // running through the city); FIFO shows every frame on its own refresh.
    const char *e = ENV("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e ? (g_settings.present_fifo ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
                          : !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    // HDR (F1 > Video > HDR): the surface offers HDR10 (10-bit PQ, BT.2020) or scRGB (FP16 linear) only while Windows
    // has the display in HDR mode. The window's images are then those; everything still draws into SDR images of the
    // window's size (sc_images / sc_views / sc_format, as before) and vk_frame_end turns them into HDR (vk_hdr_encode)
    VkSurfaceFormatKHR hf = {0};
    for (uint32_t i = 0; i < nf; i++) {
        bool pq = fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                  (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
        bool scrgb = fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && fmts[i].format == VK_FORMAT_R16G16B16A16_SFLOAT;
        if (pq || (scrgb && !hf.format)) hf = fmts[i];
    }
    g_vk.hdr_capable = hf.format != VK_FORMAT_UNDEFINED;
    bool hdr = g_vk.hdr_capable && g_settings.hdr && !ENV("LARECOMP_NO_HDR");
    VkSurfaceFormatKHR wf = hdr ? hf : sf;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = wf.format, .imageColorSpace = wf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    static const char *const modes[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
    LOG("[vk] swapchain %ux%u, %u images, %s%s\n", ext.width, ext.height, count,
        ci.presentMode < 4 ? modes[ci.presentMode] : "other present mode",
        hdr ? (wf.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ? ", HDR10" : ", HDR scRGB") : g_vk.hdr_capable ? " (HDR available)" : "");
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
        if (g_vk.sdr_mem[i]) vmaDestroyImage(g_vk.vma, g_vk.sc_images[i], g_vk.sdr_mem[i]), g_vk.sdr_mem[i] = VK_NULL_HANDLE;
        if (g_vk.hdr_views[i]) vkDestroyImageView(g_vk.dev, g_vk.hdr_views[i], NULL), g_vk.hdr_views[i] = VK_NULL_HANDLE;
    }
    if (g_vk.hdr_base) {
        vkDestroyImageView(g_vk.dev, g_vk.hdr_base_view, NULL);
        vmaDestroyImage(g_vk.vma, g_vk.hdr_base, g_vk.hdr_base_mem);
        g_vk.hdr_base = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.hdr = hdr;
    g_vk.hdr_format = wf.format;
    g_vk.hdr_space = wf.colorSpace;
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    VkImage *wimg = hdr ? g_vk.hdr_images : g_vk.sc_images;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, wimg);
    // (an SDR image to draw into: sampled by vk_hdr_encode, copied for the window shots and the overlay's base)
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = sf.format,
                             .extent = {ext.width, ext.height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = wimg[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = wf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (hdr) {
            VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.hdr_views[i]));
            VK_CHECK(vmaCreateImage(g_vk.vma, &ici, &ai, &g_vk.sc_images[i], &g_vk.sdr_mem[i], NULL));
            vi.image = g_vk.sc_images[i], vi.format = sf.format;
        }
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
    if (hdr) {
        VK_CHECK(vmaCreateImage(g_vk.vma, &ici, &ai, &g_vk.hdr_base, &g_vk.hdr_base_mem, NULL));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.hdr_base,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.hdr_base_view));
    }
}

static void recreate_swapchain(void) {
    rec_drain();  // (every queued present and submission made: the queue is idle for the wait)
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// Time this thread spends waiting here - for a frame slot's GPU work, a window image, the presentation engine -
// counts as idle for the frame pacer (pace.c), like the game's own waits: in FIFO (exclusive fullscreen
// on Windows offers no mailbox) acquire / present block until the display's refresh, and counted as busy that made
// every frame look ~16.6 ms: the game's 30 fps mode outdoors on a fast GPU (issue #44). GPU-bound frames still show
// in the game's GPU counters.
extern __thread uint64_t g_blocked_ns;  // kernel.c
#define WAITING(x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); g_blocked_ns += plat_now_ns() - w0_; } while (0)
// the same, also counted by place (dynamic resolution's log / LARECOMP_VK_PROF: 0 frame slot, 1 acquire, 2 present)
uint64_t g_vk_frame_waits[4];
#define WAITING_AT(i, x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); uint64_t w1_ = plat_now_ns() - w0_; \
                              g_blocked_ns += w1_; g_vk_frame_waits[i] += w1_; } while (0)

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

// id: frames acquired so far; secondary: the window image's rendering open now is the overlay's (vk_overlay_end), for
// secondary command buffers only; based: vk_hdr_mark_base copied the window image this frame
typedef struct { uint32_t image; VkSemaphore acquired; bool ok, secondary, based; uint64_t id; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    if (rq_stopping()) return false;  // quitting: no window image after the window closed (#40)
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    // The last present (the recorder's, record.c) must be done before the swapchain is used again - acquire and present
    // on the same swapchain are not allowed at the same time - and it may have asked for a new swapchain
    WAITING(rec_wait_presents(rec_presents_queued()));
    if (rec_present_recreate()) recreate_swapchain();
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    else if (g_vk.hdr != (g_vk.hdr_capable && g_settings.hdr && !ENV("LARECOMP_NO_HDR"))) recreate_swapchain();  // (HDR switched)
    else if (present_fifo_made != g_settings.present_fifo) recreate_swapchain();  // (Frame pacing switched)
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = ENV("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    WAITING_AT(0, vk_wait_seq(slot_seq[slot]));
    VkResult r;
    WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image));
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE,
                                          &cur_frame.image));
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    cur_frame.id++;
    cur_frame.secondary = cur_frame.based = false;
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_gpu_timer_window_start();  // (the GPU timer: this submission's work starts after that barrier, which waits for the image)
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) rec_CmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    cur_frame.secondary = false;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
}

// HDR: the window image as it is now - the game's frame, before the overlay - so vk_hdr_encode can keep what the
// overlay (FPS counter, F1 menu) draws at paper white instead of brightening it with the game's highlights
void vk_hdr_mark_base(void) {
    if (!g_vk.hdr || !cur_frame.ok || !g_vk.hdr_base) return;
    VkCommandBuffer cb = vk_cmd();
    VkImage s = g_vk.sc_images[cur_frame.image];
    vk_frame_suspend();
    image_barrier(cb, s, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(cb, g_vk.hdr_base, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {g_vk.sc_extent.width, g_vk.sc_extent.height, 1}};
    rec_CmdCopyImage(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.hdr_base, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    image_barrier(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    image_barrier(cb, g_vk.hdr_base, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_frame_resume();
    cur_frame.based = true;
}

int vk_hdr_state(void) { return g_vk.hdr ? 2 : g_vk.hdr_capable ? 1 : 0; }  // (menu.mm: F1 > Video > HDR)

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
// The window image's command buffer for direct recording (the start screen's ImGui): the recorder has recorded
// everything queued before it, and records what is queued after it.
void *vk_frame_cmd(void) {
    VkCommandBuffer cb = vk_cmd();
    rec_drain();
    return (void *)cb;
}

// ---- the overlay (menu.mm menu_render: ImGui's FPS counter, run timer, cards, the menu)
// ImGui's backend (imgui_impl_vulkan.cpp) makes its vkCmd* calls itself, on the thread that presents. With the recorder
// thread on (record.c), the window image's command buffer and its pool are the recorder's until it has caught up:
// ImGui recording into that buffer needed a full rec_drain first, every frame an overlay was up, which serialised the
// two threads (the render thread waited for the recorder to record the whole frame: ~5 ms a frame on a desktop, 20-30
// on a Steam Deck with only the FPS counter shown). Now ImGui records into a secondary command buffer from a pool of
// this file's - one per frame slot, never recorded by the recorder - and the recorder executes it in its place in the
// window image's commands: the window's rendering ends, a rendering of secondary command buffers begins on the same
// image (its contents loaded) and executes it. That rendering stays open for vk_frame_end to end: nothing is drawn
// into the window image inline after the overlay.
// A slot's buffer is recorded again (its pool reset) only once the GPU finished the submission that executed it - the
// slot comes round after vk_frame_begin waited for the window submission three frames back, so normally that is
// already so - which implies the recorder made that vkCmdExecuteCommands long before: the pool is this thread's alone
// whenever it touches it. ImGui's own objects (pipeline, buffers, descriptor sets) were only ever used by this thread.
// The recorder off, LARECOMP_VK_OVERLAY_DIRECT=1, or a second overlay in one frame: ImGui records into the window
// image's command buffer itself, after rec_drain (a no-op with the recorder off), as before.
static VkCommandPool ov_pool[FRAMES];
static VkCommandBuffer ov_cb[FRAMES];
static uint64_t ov_seq[FRAMES], ov_frame[FRAMES];  // the submission that executed the slot's buffer; the frame it was for
static VkCommandBuffer ov_open;                    // (handed out by vk_overlay_begin, until vk_overlay_end)
static int ov_direct = -1;

static bool ov_slot_ready(void) {  // (the slot's pool and buffer exist, and the buffer is free to record)
    if (!ov_pool[slot]) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = g_vk.qfam};
        if (vkCreateCommandPool(g_vk.dev, &pi, NULL, &ov_pool[slot]) != VK_SUCCESS) { ov_pool[slot] = VK_NULL_HANDLE; return false; }
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = ov_pool[slot],
                                          .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(g_vk.dev, &ai, &ov_cb[slot]) != VK_SUCCESS) {
            vkDestroyCommandPool(g_vk.dev, ov_pool[slot], NULL);
            ov_pool[slot] = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }
    if (ov_seq[slot] > vk_completed_seq()) vk_wait_seq(ov_seq[slot]);  // (normally done already: see above)
    VK_CHECK(vkResetCommandPool(g_vk.dev, ov_pool[slot], 0));
    return true;
}

// The command buffer for ImGui to record the overlay into (window_cmd: the window image's, inside its rendering), then
// vk_overlay_end with both
void *vk_overlay_begin(void *window_cmd) {
    if (ov_direct < 0) {
        const char *e = getenv("LARECOMP_VK_OVERLAY_DIRECT");
        ov_direct = !rec_on() || (e && atoi(e) != 0);
        LOG("[vk] overlay: %s\n", ov_direct ? "recorded into the window image's command buffer" : "a secondary command buffer the recorder executes");
    }
    if (ov_direct || !cur_frame.ok || ov_open || ov_frame[slot] == cur_frame.id || !ov_slot_ready()) {
        if (cur_frame.ok && cur_frame.secondary) {  // (a second overlay this frame: inline draws again, in a rendering of their own)
            vk_frame_suspend();
            vk_frame_resume();
        }
        rec_drain();  // (ImGui records into window_cmd itself: the recorder caught up and idle first)
        return window_cmd;
    }
    VkCommandBufferInheritanceRenderingInfo ir = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                                                  .colorAttachmentCount = 1, .pColorAttachmentFormats = &g_vk.sc_format,
                                                  .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkCommandBufferInheritanceInfo ii = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &ir};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                   .pInheritanceInfo = &ii};
    VK_CHECK(vkBeginCommandBuffer(ov_cb[slot], &bi));
    ov_frame[slot] = cur_frame.id;
    ov_open = ov_cb[slot];
    return (void *)ov_open;
}

void vk_overlay_end(void *window_cmd, void *cmd) {
    if (!cmd || cmd == window_cmd || (VkCommandBuffer)cmd != ov_open) return;  // (recorded into window_cmd directly)
    VkCommandBuffer sec = ov_open, cb = (VkCommandBuffer)window_cmd;
    ov_open = VK_NULL_HANDLE;
    VK_CHECK(vkEndCommandBuffer(sec));
    rec_CmdEndRendering(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
                          .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdExecuteCommands(cb, 1, &sec);
    cur_frame.secondary = true;
    ov_seq[slot] = vk_pending_seq();  // (the submission window_cmd goes in)
}

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

typedef struct { uint32_t w, h; bool bgr; char path[256]; uint8_t *px; } ShotJob;
static int shot_thread(void *arg) {
    ShotJob *j = arg;
    uint64_t n = (uint64_t)j->w * j->h;
    for (uint64_t i = 0; j->bgr && i < n; i++) { uint8_t t = j->px[i * 4]; j->px[i * 4] = j->px[i * 4 + 2]; j->px[i * 4 + 2] = t; }
    for (uint64_t i = 0; i < n; i++) j->px[i * 4 + 3] = 255;
    vk_save_png(j->path, j->px, j->w, j->h);
    free(j->px);
    free(j);
    return 0;
}

// The window stopped showing the frames: a new swapchain, and if that happens again within a minute, FIFO presents
// for the rest of the session.
static void present_recover(const char *why) {
    static uint64_t last_fix_ns;
    static int fifo_tries;
    uint64_t now = SDL_GetTicksNS();
    if (g_present_fifo && fifo_tries++ >= 3) {  // (with FIFO too: nothing more to try - no new swapchain every 3 s)
        if (fifo_tries == 4) LOG("[vk] the window still shows nothing new (%s): left as is\n", why);
        return;
    }
    bool again = last_fix_ns && now - last_fix_ns < 60000000000ull;
    LOG("[vk] the window showed nothing new for 3 s (%s): %s\n", why,
        again && !g_present_fifo ? "FIFO presents from now on" : "new swapchain");
    if (again) g_present_fifo = true;
    last_fix_ns = now;
    recreate_swapchain();
}

// What the screen really shows (#15, v0.1.8: the window froze again with the presents reported as shown - the
// compositor kept an old image of the window): once a second, a hash of the desktop over the window (platform:
// Windows, foreground window only). The game's image changing all along while the screen's stays the same for 3 s:
// present_recover. LARECOMP_NO_SCREEN_WATCH=1: off; LARECOMP_TEST_SCREEN_STALE=1: a screen that never changes
// (also hidden / unfocused windows: tests the recovery).
bool plat_window_screen_hash(void *hwnd, uint32_t *out);  // (runtime/platform: false = can't tell)
static int screen_watch(void *arg) {
    (void)arg;
    bool test = ENV("LARECOMP_TEST_SCREEN_STALE") != NULL, have = false;
    uint32_t last_scr = 0, last_img = 0;
    int stale = 0;
    for (;;) {
        SDL_Delay(1000);
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        uint32_t scr = 0, img = atomic_load(&g_img_changes);
        if (!test && !plat_window_screen_hash(hwnd, &scr)) { have = false, stale = 0; continue; }
        stale = have && scr == last_scr && img - last_img >= 5 ? stale + 1 : 0;  // (the game's image moving meanwhile)
        have = true, last_scr = scr, last_img = img;
        if (stale >= 3) { atomic_store(&g_screen_stale, true); stale = 0; }
    }
    return 0;
}

// A window that stops showing the frames (#15: a GTX 1660 Super's window froze after about an hour while the game went
// on presenting at 30 fps - sound on, F1's menu never seen; every present returned VK_SUCCESS). Checked with
// VK_KHR_present_wait while the window has the focus: nothing shown for 3 s gets a new swapchain, and if that happens
// again within a minute, FIFO presents for the rest of the session. LARECOMP_PRESENT_WATCH_ANY=1: also unfocused /
// hidden windows (a test: a hidden window shows nothing). Runs where the presents are made, right after one (the
// recorder thread, rec_QueuePresentKHR: vkWaitForPresentKHR wants the swapchain synchronised with them); what it finds
// goes to vk_frame_end (g_present_unseen), which makes the new swapchain.
void vk_present_watch(uint64_t id) {
    static uint64_t probe, probe_ns;
    static int any = -1;
    if (!g_present_wait) return;
    if (any < 0) any = ENV("LARECOMP_PRESENT_WATCH_ANY") != NULL;
    uint64_t now = SDL_GetTicksNS();
    SDL_WindowFlags f = SDL_GetWindowFlags(window);
    if (!any && (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) { probe = 0; return; }
    // (a window in the background may rightly show nothing: only said in the log, at most 10 times)
    bool focused = any || ((f & SDL_WINDOW_INPUT_FOCUS) && !(f & SDL_WINDOW_OCCLUDED));
    if (!probe) { probe = id; probe_ns = now; return; }
    VkResult r = g_present_wait(g_vk.dev, g_vk.swapchain, probe, 0);
    if (r == VK_SUCCESS) { probe = 0; return; }
    if (r != VK_TIMEOUT) {
        probe = 0;
        snprintf(g_present_unseen_why, sizeof g_present_unseen_why, "present wait %d", r);
        atomic_store(&g_present_unseen, true);
        return;
    }
    if (now - probe_ns < 3000000000ull) return;
    if (!focused) {
        static int n;
        if (n++ < 10) LOG("[vk] the window showed nothing for 3 s without the focus (present %llu not shown) - left as is\n",
                          (unsigned long long)probe);
        probe = 0;
        return;
    }
    snprintf(g_present_unseen_why, sizeof g_present_unseen_why, "present %llu not shown, now %llu",
             (unsigned long long)probe, (unsigned long long)id);
    probe = 0;
    atomic_store(&g_present_unseen, true);
}

// The window thread (ui_run) away from its event loop: Windows then shows a frozen copy of the window that takes the
// keyboard and the pads, while the game and its sound go on (#15). Said in the log with the call it is stuck in.
static _Atomic uint64_t ui_beat;  // (when ui_run last came back to its event loop)
static const char *_Atomic ui_stage;
#ifdef _WIN32
__declspec(dllimport) int __stdcall IsHungAppWindow(void *hwnd);
#endif
static void ui_watch(void) {
    static bool stuck, hung;
    static uint64_t next;
    uint64_t now = SDL_GetTicksNS(), b = atomic_load(&ui_beat);
    if (!b || now < next) return;
    next = now + 500000000ull;
    if (now - b > 2000000000ull) {
        if (!stuck) LOG("[ui] the window thread has been away for %.1f s (in %s)\n", (now - b) / 1e9, atomic_load(&ui_stage));
        stuck = true;
    } else if (stuck) {
        LOG("[ui] the window thread is back\n");
        stuck = false;
    }
#ifdef _WIN32
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    bool h = hwnd && IsHungAppWindow(hwnd);
    if (h != hung) LOG("[ui] Windows %s the window as not responding\n", h ? "shows" : "no longer shows");
    hung = h;
#endif
}

// A stuck game (#38: the log just stopped, the window frozen): no frame shown for 20 s while the window is up (not
// minimized) - said in the log, with where every thread is (plat_dump_threads), once per stall; and when
// frames come back. LARECOMP_HANG_S=<seconds> (0: off; -<seconds>: (test) the dump once, that long after the start).
static _Atomic uint64_t g_window_frames;  // (every frame shown: the game's and the start screen's, vk_frame_end)
static int hang_watch(void *arg) {
    (void)arg;
    double limit = ENV("LARECOMP_HANG_S") ? atof(ENV("LARECOMP_HANG_S")) : 20;
    if (limit < 0) {
        SDL_Delay((uint32_t)(-limit * 1000));
        LOG("[hang] (test) where each thread is:\n");
#ifdef _WIN32
        plat_dump_threads();
#endif
        return 0;
    }
    if (limit == 0) return 0;
    uint64_t last = 0, since = SDL_GetTicksNS();
    bool told = false;
    for (;;) {
        SDL_Delay(1000);
        uint64_t f = atomic_load(&g_window_frames), now = SDL_GetTicksNS();
        bool away = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED);
        if (f != last || away) {
            if (told) LOG("[hang] frames again after %.0f s\n", (now - since) / 1e9);
            last = f, since = now, told = false;
            continue;
        }
        if (told || now - since < (uint64_t)(limit * 1e9)) continue;
        told = true;
        LOG("[hang] no new frame for %.0f s (frame %llu): the game looks stuck - where each thread is:\n", limit,
            (unsigned long long)f);
#ifdef _WIN32
        plat_dump_threads();
#endif
    }
}

// (test) LARECOMP_HDR_SHOT: the HDR window image read back -> the log ([hdr] nits: the largest, and how much of the
// picture is above paper white) and build/hdr_shot.png (grey up to paper white, then red -> yellow up to the peak)
static VkSpan hdr_shot;
static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (!e) { float d = m * 5.9604644775390625e-8f; return s ? -d : d; }
    u = e == 31 ? s | 0x7F800000u | (m << 13) : s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void hdr_shot_report(const void *px) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    float paper = (float)g_settings.hdr_paper, peak = (float)g_settings.hdr_peak, maxn = 0;
    uint64_t above = 0, n = (uint64_t)w * h;
    uint8_t *out = malloc(n * 4);
    for (uint64_t i = 0; i < n; i++) {
        float c[3];
        if (g_vk.hdr_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const uint16_t *p = (const uint16_t *)px + i * 4;
            for (int k = 0; k < 3; k++) c[k] = half_to_float(p[k]) * 80;
        } else {  // (PQ, BT.2020 primaries: luminance with 2020's weights below)
            uint32_t v = ((const uint32_t *)px)[i];
            bool rgb = g_vk.hdr_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            uint32_t q[3] = {rgb ? (v >> 20) & 0x3FF : v & 0x3FF, (v >> 10) & 0x3FF, rgb ? v & 0x3FF : (v >> 20) & 0x3FF};
            for (int k = 0; k < 3; k++) {
                float e = powf(q[k] / 1023.0f, 1 / 78.84375f), l = fmaxf(e - 0.8359375f, 0) / (18.8515625f - 18.6875f * e);
                c[k] = powf(l, 1 / 0.1593017578125f) * 10000;
            }
        }
        float y = g_vk.hdr_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]
                                                                    : 0.2627f * c[0] + 0.6780f * c[1] + 0.0593f * c[2];
        float m = fmaxf(c[0], fmaxf(c[1], c[2]));
        if (m > maxn) maxn = m;
        uint8_t *o = out + i * 4;
        if (m > paper * 1.02f) {
            above++;
            float t = fminf((m - paper) / fmaxf(peak - paper, 1), 1);
            o[0] = 255, o[1] = (uint8_t)(t * 255), o[2] = 0;
        } else {
            o[0] = o[1] = o[2] = (uint8_t)fminf(y / paper * 255, 255);
        }
        o[3] = 255;
    }
    LOG("[hdr] shot: brightest %.0f nits (peak %.0f, paper white %.0f), %.1f%% of the picture above paper white\n", maxn, peak,
        paper, 100.0 * above / n);
    vk_save_png("build/hdr_shot.png", out, w, h);
    free(out);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
uint64_t renderer_present_count(void);
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    if (ab_on && ENV("LARECOMP_VK_ADDR_TEST") && renderer_present_count() == 600) {  // (test: the report without a fault)
        while (atomic_flag_test_and_set_explicit(&ab_lock, memory_order_acquire)) {}
        uint64_t big = 0, end = 0;
        for (uint32_t i = 0, n = ab_count(); i < n; i++) {
            const AbEvent *e = ab_at(i);
            if (!e->unbind && e->objtype == VK_OBJECT_TYPE_BUFFER && e->size > big) big = e->size, end = e->base + e->size;
        }
        atomic_flag_clear_explicit(&ab_lock, memory_order_release);
        LOG("[vk] address report test: %u binding events; 516 KB past the end of the largest buffer:\n", ab_n);
        if (big) ab_explain(end + 0x81000, 0x1000);
    }
    VkCommandBuffer cb = vk_cmd();
    rec_CmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    if (g_vk.hdr) {  // the SDR image drawn -> the window's HDR image
        extern void vk_hdr_encode(VkCommandBuffer cb, VkImageView sdr, VkImageView base, VkImageView dst);
        VkImage hi = g_vk.hdr_images[cur_frame.image];
        image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        image_barrier(cb, hi, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vk_hdr_encode(cb, g_vk.sc_views[cur_frame.image], cur_frame.based ? g_vk.hdr_base_view : VK_NULL_HANDLE,
                      g_vk.hdr_views[cur_frame.image]);
        static long hdr_shot_at = -1;  // (test) LARECOMP_HDR_SHOT=<window frame>: that HDR image read back (hdr_shot_report)
        if (hdr_shot_at < 0) hdr_shot_at = ENV("LARECOMP_HDR_SHOT") ? atol(ENV("LARECOMP_HDR_SHOT")) : 0;
        if (hdr_shot_at && (long)atomic_load(&g_window_frames) + 1 == hdr_shot_at) {
            uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height, bpp = g_vk.hdr_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
            image_barrier(cb, hi, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            hdr_shot = vk_readback((uint64_t)w * h * bpp);
            VkBufferImageCopy rg = {.bufferOffset = hdr_shot.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {w, h, 1}};
            rec_CmdCopyImageToBuffer(cb, hi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_shot.buf, 1, &rg);
            image_barrier(cb, hi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else {
            image_barrier(cb, hi, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
    } else {
        image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    // the present follows the submission in the recorder's order (its wait for the display is the recorder's, counted in
    // g_vk_frame_waits[2]; an out-of-date result makes the next vk_frame_begin recreate the swapchain). The present
    // watch runs there too, after it (vk_present_watch); what it finds is acted on here.
    static uint64_t present_id;  // (VK_KHR_present_id: increasing over all swapchains)
    rec_QueuePresentKHR(done_sem[img], g_vk.swapchain, img, g_present_wait ? ++present_id : 0);
    atomic_fetch_add(&g_window_frames, 1);
    ui_watch();
    static bool hw_started;
    if (!hw_started) {
        hw_started = true;
        SDL_Thread *t = SDL_CreateThread(hang_watch, "hangwatch", NULL);
        if (t) SDL_DetachThread(t);
    }
    static bool sw_started;
    if (!sw_started) {
        sw_started = true;
        SDL_Thread *t = ENV("LARECOMP_NO_SCREEN_WATCH") ? NULL : SDL_CreateThread(screen_watch, "screenwatch", NULL);
        if (t) SDL_DetachThread(t);
    }
    if (atomic_exchange(&g_present_unseen, false)) present_recover(g_present_unseen_why);
    if (atomic_exchange(&g_screen_stale, false)) present_recover("the screen kept an old image of the window");
    cur_frame.ok = false;
    if (hdr_shot.ptr) {
        vk_wait_seq(seq);
        hdr_shot_report(hdr_shot.ptr);
        hdr_shot.ptr = NULL;
    }
    if (shot_path) {
        // (the PNG is encoded on a thread of its own: done here it held the frame ~250 ms - a periodic hitch with
        // LARECOMP_SCREENSHOT_EVERY at 60 fps)
        vk_wait_seq(seq);
        ShotJob *j = malloc(sizeof *j);
        j->w = g_vk.sc_extent.width, j->h = g_vk.sc_extent.height;
        j->bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        snprintf(j->path, sizeof j->path, "%s", shot_path);
        j->px = malloc((size_t)j->w * j->h * 4);
        memcpy(j->px, shot.ptr, (size_t)j->w * j->h * 4);
        SDL_Thread *t = SDL_CreateThread(shot_thread, "png", j);
        if (t) SDL_DetachThread(t);
        else shot_thread(j);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
extern bool g_debug_frame;  // (exec.c; set in present_hooks, read in the render thread's present)
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames;
uint64_t renderer_present_count(void) { return total_frames; }  // (the game thread's frame: the HLE hooks')
// The frame whose work is being translated (rqueue.h rq_frame): "once per frame" state of the translation. Without the
// render thread, total_frames.
uint64_t renderer_exec_frame(void) { return rq_current_item() ? rq_frame() : total_frames; }
uint64_t vk_frame_counter(void) { return renderer_exec_frame(); }  //
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;
void vk_device_texture_destroyed(NvnTexture *t) { if (g_last_presented == t) g_last_presented = NULL; }  // (textures.c)

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void *backend_pool_push(void) { return NULL; }
void backend_pool_pop(void *p) { (void)p; }
void backend_represent(void) {
    if (!g_last_presented) { SDL_Delay(16); return; }
    if (rq_on()) rq_present(g_last_presented, total_frames, false);
    else backend_present_now(g_last_presented, total_frames);
}

// ---- black frame probe: each presented game image, shrunk to 32x18 on the GPU and read back a few frames later; a
// frame that comes out black between normal ones is logged ("[black]") with what was presented - the occasional black
// flash. Costs one tiny blit a frame. LARECOMP_BLACK_PROBE=0: off.
typedef struct { VkImage img; VmaAllocation im; VkBuffer buf; VmaAllocation bm; void *map; uint64_t seq, frame;
                 const void *tex; uint32_t w, h, fmt; bool used; } Probe;
static Probe probes[4];
static uint64_t probe_dark_since;

static void probe_check(Probe *p) {
    const uint8_t *px = p->map;
    double sum = 0;
    for (int i = 0; i < 32 * 18; i++) sum += 0.299 * px[4 * i] + 0.587 * px[4 * i + 1] + 0.114 * px[4 * i + 2];
    double mean = sum / (32 * 18);
    static double prev = -1;
    static uint64_t prev_frame;
    static const void *prev_tex;
    static int n;
    if (prev >= 25 && mean < 3 && p->frame == prev_frame + 1 && n++ < 60)
        LOG("[black] frame %llu presented a black image (mean %.1f, the frame before %.1f): texture %p %ux%u fmt %u%s\n",
            (unsigned long long)p->frame, mean, prev, p->tex, p->w, p->h, p->fmt,
            p->tex != prev_tex ? " - another texture than the frame before" : "");
    if (prev < 3 && mean >= 25 && probe_dark_since && p->frame - probe_dark_since <= 6 && n < 60)
        LOG("[black] ... normal again at frame %llu (%llu dark frames)\n", (unsigned long long)p->frame,
            (unsigned long long)(p->frame - probe_dark_since));
    if (mean < 3 && prev >= 3) probe_dark_since = p->frame;
    prev = mean, prev_frame = p->frame, prev_tex = p->tex;
    // [freeze]: the game's own image the same for 10 s (a menu or a still screen can be; #15: tells "the game stopped
    // drawing" from "the window stopped showing what it draws")
    uint32_t h = 2166136261u;
    for (int i = 0; i < 32 * 18 * 4; i++) h = (h ^ px[i]) * 16777619u;
    static uint32_t last_h;
    static uint64_t same_ns, same_frame;
    static bool frozen;
    static int nf;
    uint64_t now = SDL_GetTicksNS();
    {   // (screen_watch: only a change of 10% or more of the samples - one a screen grab can't miss)
        static uint8_t prev_px[32 * 18 * 4];
        int moved = 0;
        for (int i = 0; i < 32 * 18; i++) {
            int d = 0;
            for (int c = 0; c < 3; c++) d += abs((int)px[4 * i + c] - prev_px[4 * i + c]);
            moved += d > 24;
        }
        memcpy(prev_px, px, sizeof prev_px);
        if (moved >= 32 * 18 / 10) atomic_fetch_add(&g_img_changes, 1);
    }
    if (h != last_h || !same_ns) {
        if (frozen && nf <= 30) LOG("[freeze] the game's image changes again at frame %llu\n", (unsigned long long)p->frame);
        frozen = false, last_h = h, same_ns = now, same_frame = p->frame;
    } else if (!frozen && now - same_ns > 10000000000ull) {
        frozen = true;
        if (nf++ < 30)
            LOG("[freeze] the game's image hasn't changed for 10 s (frames %llu-%llu, mean %.1f, texture %p %ux%u)\n",
                (unsigned long long)same_frame, (unsigned long long)p->frame, mean, p->tex, p->w, p->h);
    }
}

static void black_probe(TexBackend *b, const void *tex, uint64_t frame) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("LARECOMP_BLACK_PROBE"); on = !e || atoi(e) != 0; }
    if (!on || !b->image || (b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) return;
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < 4; i++) {  // (in frame order: the oldest first)
        Probe *q = NULL;
        for (int k = 0; k < 4; k++)
            if (probes[k].used && probes[k].seq <= done && (!q || probes[k].frame < q->frame)) q = &probes[k];
        if (!q) break;
        probe_check(q);
        q->used = false;
    }
    Probe *p = NULL;
    for (int k = 0; k < 4 && !p; k++) if (!probes[k].used) p = &probes[k];
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    if (!p->img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {32, 18, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &p->img, &p->im, NULL) != VK_SUCCESS) { on = 0; return; }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 32 * 18 * 4,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ba = {.usage = VMA_MEMORY_USAGE_AUTO,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ba, &p->buf, &p->bm, &info) != VK_SUCCESS) { on = 0; return; }
        p->map = info.pMappedData;
        vk_image_init_layout(cb, p->img, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {32, 18, 1}}};
    rec_CmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, p->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    rec_CmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {32, 18, 1}};
    rec_CmdCopyImageToBuffer(cb, p->img, VK_IMAGE_LAYOUT_GENERAL, p->buf, 1, &rg);
    rec_CmdPipelineBarrier2(cb, &di);
    p->used = true, p->seq = vk_pending_seq(), p->frame = frame, p->tex = tex;
    p->w = b->width, p->h = b->height, p->fmt = (uint32_t)b->format;
}

// (LARECOMP_VK_PROF) the present's time by part, per frame in the fps line ("present/frame"): submit = the frame's
// last work submitted, acquire = frame slot + window image (waits included), scale = output scale / widescreen /
// dynamic resolution, blit = the game image into the window (FSR / blit / AA recording), overlay = menu_render (ImGui),
// end = the window image's submission + vkQueuePresentKHR, rest = test hooks and the fps line
enum { PP_SUBMIT, PP_ACQUIRE, PP_SCALE, PP_BLIT, PP_OVERLAY, PP_END, PP_REST, PP_N };
static int pp_on = -1;
static uint64_t pp_ns[PP_N], pp_frames, pp_t;
static void pp_mark(int part) {
    if (!pp_on) return;
    uint64_t t = SDL_GetTicksNS();
    pp_ns[part] += t - pp_t;
    pp_t = t;
}
static const char *pp_text(void) {
    static char s[192];
    s[0] = 0;
    if (!pp_on || !pp_frames) return s;
    double f = (double)pp_frames * 1e6;
    snprintf(s, sizeof s, " | present/frame submit %.3f acquire %.3f scale %.3f blit %.3f overlay %.3f end %.3f rest %.3f ms",
             pp_ns[PP_SUBMIT] / f, pp_ns[PP_ACQUIRE] / f, pp_ns[PP_SCALE] / f, pp_ns[PP_BLIT] / f, pp_ns[PP_OVERLAY] / f,
             pp_ns[PP_END] / f, pp_ns[PP_REST] / f);
    memset(pp_ns, 0, sizeof pp_ns);
    pp_frames = 0;
    return s;
}

// The GPU side of a present (the render thread when it's on, rqueue.c): the present pass, the overlay, shots.
// frame: the number of this present (the game thread's frame when it was queued).
void backend_present_now(NvnTexture *t, uint64_t frame) {
    if (pp_on < 0) pp_on = getenv("LARECOMP_VK_PROF") != NULL;
    if (pp_on) pp_t = SDL_GetTicksNS();
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    // the HD texture switch (F1 > Textures, applied while playing): between frames, on the thread that uploads the
    // textures (the render thread when it is on)
    extern void vk_hd_switch_frame(void);
    vk_hd_switch_frame();
    black_probe(b, t, frame);
    // the frame's work goes to the GPU now, in a submission of its own: it starts while we wait for the window's image.
    // Its GPU time is counted, and the window image's submission's from after its wait (exec.c gpu_timer_add)
    renderer_submit();
    pp_mark(PP_SUBMIT);
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    pp_mark(PP_ACQUIRE);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(frame);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // GPU time since the last frame (exec.c's GPU timer; -1 without one: dynamic resolution goes by frame times):
        // the game's submissions and the window image's (LARECOMP_DYNRES_COUNT_PRESENT=0: the game's only, as before)
        static int count_present = -1;
        if (count_present < 0) count_present = !getenv("LARECOMP_DYNRES_COUNT_PRESENT") || atoi(getenv("LARECOMP_DYNRES_COUNT_PRESENT"));
        static uint64_t sp_prev[4], wt_prev[4], cw_prev;
        static bool sp_have;
        uint64_t sp[4], wt[4];
        for (int i = 0; i < 4; i++) wt[i] = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);  // ([2], [3]: the recorder's)
        double game_ms = -1, win_ms = -1;
        if (vk_gpu_split_ns(sp)) {
            uint64_t cw = vk_cum_wait_ns();
            if (sp_have) {
                double game = (sp[0] - sp_prev[0]) / 1e6, win = (sp[1] - sp_prev[1]) / 1e6;
                game_ms = game, win_ms = count_present ? win : -1;
                double slotw = (wt[0] - wt_prev[0]) / 1e6;
                DynSplit ds = {game, win, (sp[2] - sp_prev[2]) / 1e6, (double)(sp[3] - sp_prev[3]), slotw,
                               (wt[1] - wt_prev[1]) / 1e6, (wt[2] - wt_prev[2]) / 1e6,
                               fmax(0, (cw - cw_prev) / 1e6 - slotw), count_present != 0};
                output_scale_note_split(&ds);
            }
            memcpy(sp_prev, sp, sizeof sp);
            memcpy(wt_prev, wt, sizeof wt_prev);
            cw_prev = cw;
            sp_have = true;
        }
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, game_ms, win_ms, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        output_scale_log((unsigned long long)frame, dw, dh, &os);  // (diagnostics: [scale] on a change)
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(frame, shape, crop, &vx, &vy, &vw, &vh);
        {   // the game's own crop of its window texture (handheld: 1280x720 of a 1920x1080 texture): only that part is the picture
            extern int g_present_crop[4], g_present_tex[2];
            const int *wc = g_present_crop, tw = g_present_tex[0], th = g_present_tex[1];
            if (!ENV("LARECOMP_NO_WINDOW_CROP") && wc[2] > 0 && wc[3] > 0 && tw > 0 && th > 0 && (wc[2] < tw || wc[3] < th || wc[0] || wc[1])) {
                float cx = (float)wc[0] / tw, cy = (float)wc[1] / th, cw = (float)wc[2] / tw, ch = (float)wc[3] / th;
                crop[0] = cx + crop[0] * cw; crop[1] = cy + crop[1] * ch; crop[2] *= cw; crop[3] *= ch;
            }
        }
        widescreen_set_factor((float)shape);
        extern void renderer_set_alloc_scale(float ax, float ay);
        renderer_set_alloc_scale((float)os.ax, (float)os.ay);
        renderer_set_scale((float)sx, (float)sy);
        extern void vk_sampler_settings(double lines);
        vk_sampler_settings(os.lines);  // (the mip bias of a frame drawn smaller than shown; F1's texture filtering)
        pp_mark(PP_SCALE);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        pp_mark(PP_BLIT);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)frame + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        // (ImGui records through vk_overlay_begin / vk_overlay_end, when it draws anything)
        vk_hdr_mark_base();
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        pp_mark(PP_OVERLAY);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)frame + 1 == win_shot;
        vk_frame_end(shot ? "build/window.png" : NULL);
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) exit_after_shot();
    } else {
        renderer_submit();
    }
    pp_mark(PP_END);
    pp_frames++;
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(frame, SDL_GetTicksNS() - t0);
    frames++;
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)frame) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)frame);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)frame < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)frame == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)frame);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    extern volatile int g_shot_request;  // F10 (runtime/hle/input.c): the game image -> screenshots/shot_<frame>.png
    if (g_shot_request) {
        g_shot_request = 0;
        char path[256];
        mkdir("screenshots", 0755);
        snprintf(path, sizeof path, "screenshots/shot_%05llu.png", (unsigned long long)frame);
        renderer_save_texture_tex(b, path);
        LOG("[shot] frame %llu -> %s\n", (unsigned long long)frame, path);
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && frame % shot_every == 0 && (long)frame >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)frame);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) exit_after_shot();
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    static double hitch_ms = -1;  // LARECOMP_LOG_HITCH_MS=<ms>: every present gap over that (tests: hitches per run)
    if (hitch_ms < 0) hitch_ms = getenv("LARECOMP_LOG_HITCH_MS") ? atof(getenv("LARECOMP_LOG_HITCH_MS")) : 0;
    if (hitch_ms > 0 && prev_present > 0 && (now - prev_present) * 1000 > hitch_ms)
        LOG("[hitch] t=%.2f frame %llu gap %.0f ms\n", now, (unsigned long long)frame, (now - prev_present) * 1000);
    prev_present = now;
    pp_mark(PP_REST);  // (the fps line itself is not counted)
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s%s)\n", now, (unsigned long long)frame,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), renderer_stats(), pp_text());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
    static double mem_at = 60.0;  // (memory in steady state: a minute in, then every 10 minutes)
    if (now >= mem_at) { vk_log_memory("playing"); mem_at = now + 600.0; }
    // once a minute, what the process holds (a leak: it grows over a long session). Which side - the guest's memory or
    // the rest of the process (the renderer's host-side caches, the driver) - only with LARECOMP_MEM_SPLIT=1 (Windows:
    // counting the guest's walks its whole window, a dropped frame each time; platform/windows.c plat_mem_usage)
    static double last_mem;
    uint64_t proc, guest;
    if (now - last_mem >= 60 && plat_mem_usage(&proc, &guest)) {
        last_mem = now;
        extern int kernel_live_threads(void), vk_textures_live(void);
        extern uint32_t nvn_textures_count(void);
        extern void vk_exec_counts(uint32_t *progs, uint32_t *pipes);
        uint32_t progs, pipes;
        vk_exec_counts(&progs, &pipes);
        char what[128];  // (#70: 7.5 -> 26 GB over 2.3 h on an RTX 3090 - which of these grows with it)
        snprintf(what, sizeof what, "%d guest threads, %u textures (%d on the GPU), %u shader programs, %u pipelines",
                 kernel_live_threads(), nvn_textures_count(), vk_textures_live(), progs, pipes);
        if (guest)
            LOG("[mem] t=%.0f process %llu MB: guest %llu MB + host %llu MB (%s)\n", now,
                (unsigned long long)(proc >> 20), (unsigned long long)(guest >> 20), (unsigned long long)((proc - guest) >> 20), what);
        else LOG("[mem] t=%.0f process %llu MB (%s)\n", now, (unsigned long long)(proc >> 20), what);
    }
}

// The game thread's side of a present: the frame counters and the per-frame hooks (input, pacing, test hooks).
static void present_hooks(void) {
    total_frames++;
    pace_frame();  // (pace.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;  // LARECOMP_SIM_SLOW_FRAME_MS=<ms>: sleep per presented frame (simulate a slow machine)
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SIM_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SIM_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = ENV("LARECOMP_SLOW_FRAME_UNTIL") ? atol(ENV("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    { extern void pw_check(const char *when); pw_check("at present"); }  // (debug: LARECOMP_POOL_WATCH, exec.c)
    {   // (test) LARECOMP_TEST_BAD_CODE=<frame>: as a device fault with an invalid instruction
        static long at = -1;
        if (at < 0) at = ENV("LARECOMP_TEST_BAD_CODE") ? atol(ENV("LARECOMP_TEST_BAD_CODE")) : 0;
        if (at && (long)total_frames >= at) { vk_bad_code_note(); FATAL("[vk] test: device lost\n"); }
    }
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = ENV("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = ENV("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
}

void backend_present(NvnTexture *t) {
    g_last_presented = t;
    uint64_t frame = total_frames, t0 = plat_now_ns();
    if (rq_on()) rq_present(t, frame, true);
    else backend_present_now(t, frame);
    uint64_t t1 = plat_now_ns();
    g_present_ns += t1 - t0;  // (LARECOMP_LOG_PACE, pace.c)
    present_hooks();
    g_hooks_ns += plat_now_ns() - t1;
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// The end of a validation run (LARECOMP_VK_VALIDATION, after backend_quit: the GPU idle, the pipeline cache kept):
// the debug messenger goes, then the process ends at once - no vkDestroyDevice / vkDestroyInstance, no atexit
// handlers, no DLL teardown. _exit() alone isn't enough on Windows: it ends in ExitProcess, which runs the layer's
// and the driver's DLL teardown after every other thread is gone, and with the Khronos layer on NVIDIA that left the
// process hung there and unkillable (one thread waiting, gigabytes of the layer's state).
static _Noreturn void validation_exit(void) {
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m = messenger;
        messenger = VK_NULL_HANDLE;
        if (f) f(g_vk.inst, m, NULL);
    }
    LOG("[vk] validation run: ending without GPU teardown\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
    _Exit(0);
}

// LARECOMP_EXIT_AFTER_SHOT: the test run ends with its shot. As it always did (_exit: no cache save), except under
// validation: backend_quit, then validation_exit.
static void exit_after_shot(void) {
    fflush(stderr);
    if (validating) {
        backend_quit();
        validation_exit();
    }
    _exit(0);
}

// Quitting, before exit() (the window closed; also from exit() itself, quick_exit_handler): the render thread stops
// between items, so no image is acquired or presented any more; the recorder thread stops making driver calls (the
// packets it still had are dropped, record.c rec_quit) and is idle; then the GPU finishes and the pipeline cache is
// kept. All of it before exit() runs the atexit handlers and the libraries' destructors: a Vulkan layer loaded with
// the instance (the Steam overlay on the Steam Deck, #40) tears its swapchain state down there, and a present from
// the render thread after that crashed in vkQueuePresentKHR. The swapchain, surface and window are never destroyed
// explicitly: the process ends with them.
void backend_quit(void) {
    static atomic_flag once = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&once)) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] quit: stopping the render thread\n");
    rq_stop();
    if (trace) LOG("[vk] quit: stopping the recorder\n");
    rec_quit();  // (what it still had is dropped, not run: no submission or present after this; the queue idle for the wait)
    if (!g_vk.dev) return;
    if (trace) LOG("[vk] quit: waiting for the GPU\n");
    vkDeviceWaitIdle(g_vk.dev);
    if (trace) LOG("[vk] quit: saving the pipeline cache\n");
    vk_pipeline_cache_save();
}

// exit() from any thread: backend_quit (when the window close didn't already), then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    backend_quit();
    if (validating) validation_exit();
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    settings_load();
    input_apply_background();  // controllers in the background: "Allow background inputs" (Controls tab)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (ENV("LARECOMP_WINDOW")) sscanf(ENV("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (ENV("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = ENV("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (ENV("LARECOMP_INPUT") || ENV("LARECOMP_DOORJUMP_TEST") || ENV("LARECOMP_PLACE") ||
                                            ENV("LARECOMP_HOST_INPUT") || ENV("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("ZA Recomp", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !ENV("LARECOMP_WINDOW") && !ENV("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }
bool ui_window_focused(void) { return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS); }  // (any thread, for a log)

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    rq_drain();  // (the render thread: everything queued ran; it idles while the game is down)
    rec_drain();  // (the recorder: everything the render thread queued is recorded and submitted)
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    if (rq_stopping()) return;
    rq_drain();
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

// [window]: what happens to the window (focus, minimised, covered, display) - #15: a window that stops responding
static void window_event_log(const SDL_Event *e) {
    static int n;
    const char *what = NULL;
    switch (e->type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED: what = "focus gained"; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: what = "focus lost"; break;
    case SDL_EVENT_WINDOW_MINIMIZED: what = "minimised"; break;
    case SDL_EVENT_WINDOW_RESTORED: what = "restored"; break;
    case SDL_EVENT_WINDOW_MAXIMIZED: what = "maximised"; break;
    case SDL_EVENT_WINDOW_SHOWN: what = "shown"; break;
    case SDL_EVENT_WINDOW_HIDDEN: what = "hidden"; break;
    case SDL_EVENT_WINDOW_OCCLUDED: what = "covered"; break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: what = "moved to another display"; break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: what = "fullscreen"; break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: what = "windowed"; break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: what = "resized"; break;
    default: return;
    }
    if (n++ < 300) LOG("[window] %s (t=%.1f)\n", what, SDL_GetTicks() / 1000.0);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        atomic_store(&ui_beat, SDL_GetTicksNS());
        atomic_store(&ui_stage, "waiting for events");
        while (SDL_WaitEventTimeout(&e, 16)) {
            input_rumble_poll();
            atomic_store(&ui_beat, SDL_GetTicksNS());
            window_event_log(&e);
            if (e.type == SDL_EVENT_QUIT) {
                input_rumble_stop();
                input_led_stop();
                LOG("window closed\n");
                backend_quit();  // (before exit(): see there)
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            input_cursor_event(&e);
            atomic_store(&ui_stage, "menu_handle_event");
            if (menu_handle_event(&e)) continue;
            atomic_store(&ui_stage, "input_handle_event");
            input_handle_event(&e);
            atomic_store(&ui_stage, "waiting for events");
        }
        static int stall = -1;  // (test: LARECOMP_TEST_UI_STALL=<s>: the window thread away for 8 s, that long after start)
        if (stall < 0) stall = ENV("LARECOMP_TEST_UI_STALL") ? atoi(ENV("LARECOMP_TEST_UI_STALL")) : 0;
        if (stall > 0 && SDL_GetTicks() > stall * 1000ull) { stall = 0; atomic_store(&ui_stage, "LARECOMP_TEST_UI_STALL"); SDL_Delay(8000); }
        atomic_store(&ui_stage, "menu_apply_window_requests");
        menu_apply_window_requests(window);
        atomic_store(&ui_stage, "input_mouse_update");
        input_mouse_update(window);
        input_rumble_poll();
        input_led_poll();
    }
}

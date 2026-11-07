"""Exercise the production validation setup/callback with simulated Vulkan failures, without a GPU."""
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
source = subprocess.check_output(['git','show',f'{args.ref}:{path}'],cwd=root,text=True) if args.ref else (root/path).read_text()
production = source[source.index('static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb('):source.index('static bool has_ext(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef uint32_t Uint32;
static struct { VkInstance inst; } g_vk;
static VkDebugUtilsMessengerEXT messenger;
static const char *scenario;
static unsigned instance_calls, callback_calls;
static bool requested_validation;
#define LOG(...) fprintf(stderr,__VA_ARGS__)
// (ZA Recomp's create_instance: ENV caches getenv, and DLAA's NGX may add instance extensions - none here)
#define ENV(name) getenv(name)
static void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) { (void)ext; (void)n; (void)max; }
// (and asks the loader for HDR colour spaces / debug utils for the address binding report: none offered here)
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *n, VkExtensionProperties *p) { (void)layer; (void)p; *n = 0; return VK_SUCCESS; }
static void ab_messenger_create(void) {}
#define FATAL(...) do { fprintf(stderr,__VA_ARGS__); fflush(stderr); _Exit(19); } while (0)
static const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *extensions[]={"VK_KHR_surface"}; *count=1; return extensions;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)allocator; instance_calls++;
    assert(info->pApplicationInfo->apiVersion==VK_API_VERSION_1_3);
    if (info->enabledLayerCount) {
        requested_validation=true;
        assert(info->enabledLayerCount==1 && !strcmp(info->ppEnabledLayerNames[0],"VK_LAYER_KHRONOS_validation"));
        assert(info->enabledExtensionCount==2 && !strcmp(info->ppEnabledExtensionNames[1],VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        if (!strcmp(scenario,"missing-layer")) return VK_ERROR_LAYER_NOT_PRESENT;
    } else assert(info->enabledExtensionCount==1);
    *instance=(VkInstance)(uintptr_t)1; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_callback(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                                     const VkAllocationCallbacks *allocator, VkDebugUtilsMessengerEXT *output) {
    (void)allocator; callback_calls++;
    assert(instance && info->pfnUserCallback);
    if (!strcmp(scenario,"callback-error")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *output=(VkDebugUtilsMessengerEXT)(uintptr_t)2; return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    assert(instance && !strcmp(name,"vkCreateDebugUtilsMessengerEXT"));
    return !strcmp(scenario,"missing-callback") ? NULL : (PFN_vkVoidFunction)create_callback;
}
''' + production + r'''
static void cleanup(void) { puts("cleanup"); }
int main(int argc, char **argv) {
    assert(argc==2); scenario=argv[1]; atexit(cleanup);
    create_instance();
    bool wanted=getenv("LARECOMP_VK_VALIDATION") || getenv("LARECOMP_VK_VALIDATION_STOP");
    assert(requested_validation==wanted);
    if (!strcmp(scenario,"missing-layer")) assert(instance_calls==2 && !callback_calls);
    else assert(instance_calls==1);
    VkDebugUtilsMessengerCallbackDataEXT message={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
                                                  .pMessage="synthetic warning"};
    if (!strcmp(scenario,"limit-error"))
        for (int i=0;i<501;i++) debug_cb(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,0,&message,NULL);
    if (!strcmp(scenario,"warning")) debug_cb(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,0,&message,NULL);
    if (!strcmp(scenario,"error") || !strcmp(scenario,"limit-error")) {
        message.pMessage="synthetic validation error";
        debug_cb(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,0,&message,NULL);
    }
    puts("completed");
}
'''
build = (root/'build').resolve(); build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='vulkan-validation-',dir=build) as directory:
    temp=Path(directory).resolve(); assert temp.is_relative_to(build)
    cfile=temp/'validation.c'; cfile.write_text(harness)
    exe=temp/('validation.exe' if os.name=='nt' else 'validation')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O1',str(cfile),'-o',str(exe)],check=True)
    cases=[('success','off',0,''),('success','strict',0,''),('warning','strict',0,''),('error','optional',0,''),
           ('error','strict',1,'synthetic validation error'),('limit-error','strict',1,'synthetic validation error'),
           ('success','strict-alone',0,''),
           ('missing-layer','optional',0,''),('missing-layer','strict',1,'required validation'),
           ('missing-callback','optional',0,''),('missing-callback','strict',1,'required validation callback'),
           ('callback-error','optional',0,''),('callback-error','strict',1,'required validation callback')]
    for scenario,mode,status,diagnostic in cases:
        env={k:v for k,v in os.environ.items() if k not in ('LARECOMP_VK_VALIDATION','LARECOMP_VK_VALIDATION_STOP')}
        if mode.startswith('strict'): env['LARECOMP_VK_VALIDATION_STOP']='1'
        if mode in ('optional','strict'): env['LARECOMP_VK_VALIDATION']='1'
        run=subprocess.run([str(exe),scenario],env=env,capture_output=True,text=True,timeout=5)
        assert run.returncode==status,(scenario,mode,run)
        assert run.stdout==('' if status else 'completed\ncleanup\n'),(scenario,mode,run)
        assert diagnostic in run.stderr,(scenario,mode,run)
print('PASS: required validation, optional fallback, warning/error policy, log limit and no cleanup after strict failure')

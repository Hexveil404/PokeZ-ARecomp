"""A failed timeline query or submission wait must not permit resource reuse. No GPU is needed."""
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
def read(path):
    return subprocess.check_output(['git', 'show', f'{args.ref}:{path}'], cwd=root, text=True) if args.ref else (root/path).read_text()
source = read('runtime/nvn/vulkan/exec.c')
header = read('runtime/nvn/vulkan/vk.h')
_q = source.index('uint64_t vk_completed_seq(void) {')
query = source[_q:source.index('\n}\n', _q) + 3]  # (the function alone: ZA Recomp keeps device-lost reports after it)
wait = source[source.index('void vk_wait_seq(uint64_t seq) {'):source.index('uint64_t vk_pending_seq(void)')]
start = header.index('#define VK_CHECK(x)')
check = header[start:header.index('\n\n', start)]
harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int VkResult;
enum { VK_SUCCESS=0, VK_TIMEOUT=2, VK_ERROR_OUT_OF_HOST_MEMORY=-1, VK_ERROR_DEVICE_LOST=-4,
       VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO=1000207004 };
typedef struct { int sType; unsigned semaphoreCount; const int *pSemaphores; const uint64_t *pValues; } VkSemaphoreWaitInfo;
static struct { int dev, timeline; } g_vk={1,2};
static unsigned calls, faults, dumps, accounts;
static uint64_t resolved, stat_wait_ns, cum_wait, ticks;
static const char *wait_why;
static VkResult result;
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); _Exit(17); } while (0)
static uint64_t SDL_GetTicksNS(void) { return ++ticks; }
static void wait_account(uint64_t elapsed) { assert(elapsed); accounts++; wait_why=NULL; }
static void cq_resolve_done(uint64_t seq) { resolved=seq; }
void vk_device_fault_report(void) { faults++; }
void vk_devlost_dump(void) { dumps++; }
void rq_watch(const char *what, uint64_t arg) { (void)what; (void)arg; }
static void ir_dump(void) {}  // (ZA Recomp: the indirect draws in flight, on a timeout or device loss)  // (the render thread's hang dump view)
// the recorder thread (record.c): what the translating thread queued (submit_seq), what the driver has (rec_submitted)
static pthread_mutex_t seq_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t submit_seq=100, rec_sub=100, rec_waited;
uint64_t rec_submitted(void) { return rec_sub; }
void rec_wait_submitted(uint64_t seq) { assert(seq<=submit_seq && seq>rec_sub); rec_waited=seq; rec_sub=seq; }
static VkResult vkGetSemaphoreCounterValue(int device, int semaphore, uint64_t *value) {
    assert(device==1 && semaphore==2); calls++;
    *value=result==VK_SUCCESS ? 42 : 999; // output is unusable after an API error
    return result;
}
static VkResult vkWaitSemaphores(int device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    assert(device==1 && info->semaphoreCount==1 && *info->pSemaphores==2 && *info->pValues==9);
    assert(timeout==10000000000ull); calls++;
    return result;
}
''' + check + '\n' + query + '\n' + wait + r'''
int main(int argc, char **argv) {
    assert(argc==2);
    const char *mode=argv[1];
    if (!strcmp(mode,"query-ok")) {
        assert(vk_completed_seq()==42 && calls==1);
    } else if (!strncmp(mode,"query-",6)) {
        result=!strcmp(mode,"query-lost") ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
        vk_completed_seq();
        puts("accepted a failed timeline query");
    } else if (!strcmp(mode,"wait-zero")) {
        wait_why="no submission"; result=VK_TIMEOUT;
        vk_wait_seq(0);
        assert(!calls && !accounts && !resolved && !wait_why);
    } else if (!strcmp(mode,"wait-recorder")) {  // not handed to the driver yet: the recorder first, then the GPU
        rec_sub=3;
        vk_wait_seq(9);
        assert(rec_waited==9 && calls==1 && accounts==1 && resolved==9);
    } else if (!strcmp(mode,"wait-unqueued")) {  // nothing queued at all: no wait
        submit_seq=0; rec_sub=0; wait_why="unqueued";
        vk_wait_seq(9);
        assert(!rec_waited && !calls && !accounts && !resolved && !wait_why);
    } else {
        result=!strcmp(mode,"wait-timeout") ? VK_TIMEOUT : !strcmp(mode,"wait-lost") ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        vk_wait_seq(9);
        if (result!=VK_SUCCESS) puts("reused in-flight resources after a failed wait");
        else assert(calls==1 && accounts==1 && resolved==9 && stat_wait_ns && cum_wait && !faults && !dumps);
    }
    puts("completed");
}
'''
build = (root/'build').resolve(); build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='vulkan-sync-', dir=build) as directory:
    temp = Path(directory).resolve(); assert temp.is_relative_to(build)
    cfile = temp/'sync.c'; cfile.write_text(harness)
    exe = temp/('sync.exe' if os.name=='nt' else 'sync')
    subprocess.run(shlex.split(os.environ.get('CC','clang')) + ['-std=gnu11','-O1',str(cfile),'-o',str(exe)],check=True)
    failures=[]
    recorder = 'rec_submitted()' in wait  # (the recorder thread's branch: its two cases too)
    ok = ('query-ok','wait-zero','wait-ok') + (('wait-recorder','wait-unqueued') if recorder else ())
    for mode in ok + ('query-lost','query-memory','wait-timeout','wait-lost'):
        run = subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5)
        success = mode in ok
        if run.returncode!=(0 if success else 17) or run.stdout!=('completed\n' if success else ''):
            failures.append((mode,run)); continue
        if mode=='wait-timeout': assert '10 s' in run.stderr and '9' in run.stderr,run
    assert not failures, failures
print('PASS: successful/empty waits, successful counter queries, timeout and API-error termination')

"""Test the actual Windows process implementation without the renderer/guest runtime."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

if os.name != 'nt':
    raise SystemExit('Windows-only test')
root = Path(__file__).resolve().parent.parent
source = (root/'runtime/platform/windows.c').read_text(encoding='utf-8')
quote = source[source.index('static void append_arg('):source.index('// No exec() on Windows:')]
process = source[source.index('// ---- child processes'):source.index('bool plat_remove_tree(')]
harness = r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "platform.h"
#include <stdarg.h>
void rt_launcher_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }  // (ZA Recomp's spawn logs to launcher.log)
static bool reject_job;
static BOOL assign_job(HANDLE job, HANDLE process) {
    if (reject_job) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return AssignProcessToJobObject(job, process);
}
#define AssignProcessToJobObject assign_job
'''+quote+process+r'''
#undef AssignProcessToJobObject
static DWORD read_pid(const char *path) {
    for(int i=0;i<250;++i) {
        FILE *f=fopen(path,"rb"); DWORD pid=0;
        if(f) { fscanf(f,"%lu",&pid); fclose(f); if(pid)return pid; }
        Sleep(20);
    }
    assert(!"child did not start"); return 0;
}
int main(int argc,char **argv) {
    char exe[1024]; assert(GetModuleFileNameA(NULL,exe,sizeof exe));
    if(argc>1&&!strcmp(argv[1],"--sleep")) { Sleep(30000); return 0; }
    if(argc>1&&!strcmp(argv[1],"--code")) { ExitProcess(argc>2?7:0xc0000005u); }
    if(argc>2&&!strcmp(argv[1],"--parent")) {
        // A real compiler's child does not call plat_spawn itself.
        char cmd[2048]=""; append_arg(cmd,sizeof cmd,exe);append_arg(cmd,sizeof cmd,"--sleep");
        STARTUPINFOA si={.cb=sizeof si}; PROCESS_INFORMATION pi;
        assert(CreateProcessA(exe,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
        FILE *f=fopen(argv[2],"wb");assert(f);fprintf(f,"%lu",pi.dwProcessId);fclose(f);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);Sleep(30000);return 0;
    }
    assert(argc==2);
    char a[1200],b[1200];snprintf(a,sizeof a,"%s/first.pid",argv[1]);snprintf(b,sizeof b,"%s/second.pid",argv[1]);
    const char *one[]={exe,"--parent",a,NULL},*two[]={exe,"--parent",b,NULL};
    PlatProc first=plat_spawn(one,NULL,false),second=plat_spawn(two,NULL,false);assert(first&&second);
    HANDLE child1=OpenProcess(SYNCHRONIZE,FALSE,read_pid(a)),child2=OpenProcess(SYNCHRONIZE,FALSE,read_pid(b));
    assert(child1&&child2&&plat_proc_wait(first,false)==-1&&plat_proc_wait(second,false)==-1);
    plat_proc_kill(first);
    assert(WaitForSingleObject(child1,5000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(child2,0)==WAIT_TIMEOUT&&plat_proc_wait(second,false)==-1);
    plat_proc_kill(second);assert(WaitForSingleObject(child2,5000)==WAIT_OBJECT_0);
    CloseHandle(child1);CloseHandle(child2);
    const char *code[]={exe,"--code","7",NULL},*crash[]={exe,"--code",NULL};
    assert(plat_proc_wait(plat_spawn(code,NULL,false),true)==7);
    assert(plat_proc_wait(plat_spawn(crash,NULL,false),true)==255);
    reject_job=true;
    assert(plat_proc_wait(plat_spawn(code,NULL,false),true)==7);
    const char *sleeper[]={exe,"--sleep",NULL};
    PlatProc fallback=plat_spawn(sleeper,NULL,false);
    assert(fallback && !((WinProcess *)fallback)->job && plat_proc_wait(fallback,false)==-1);
    plat_proc_kill(fallback);
    puts("Windows process trees: isolated cancellation, crash status and fallback without job assignment passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='las process test ') as tmp:
    tmp = Path(tmp).resolve()
    code, exe = tmp/'test.c', tmp/'process-test.exe'
    code.write_text(harness, encoding='utf-8')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-Iruntime',str(code),'-o',str(exe)],
                   cwd=root,check=True)
    subprocess.run([str(exe),str(tmp)],check=True,timeout=20)

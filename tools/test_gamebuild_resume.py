"""Exercise the production C build scheduler with tiny generated programs and a real compiler.

Only process launching/resource lookup are bridged to Python. Generation, cancellation,
receipts, repair, linking and publication run through runtime/gamebuild.c and the real cache.
No game assets or downloaded toolchains are needed (CC/CXX default to clang/clang++).
"""
import ctypes as ct
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
BRIDGE = r'''
#include "platform.h"
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <cstdio>
static std::string root;
static intptr_t (*start_cb)(const char *const *, const char *, bool);
static int (*wait_cb)(intptr_t, bool);
static void (*kill_cb)(intptr_t);
extern "C" void test_init(const char *r, intptr_t (*s)(const char *const *,const char *,bool),
                          int (*w)(intptr_t,bool), void (*k)(intptr_t)) { root=r;start_cb=s;wait_cb=w;kill_cb=k; }
extern "C" bool rt_resource(const char *,const char *dev,char *out,size_t n) {
    if(!dev)return false;
    std::string p=root+"/"+dev;
    return std::filesystem::exists(std::filesystem::u8path(p)) && snprintf(out,n,"%s",p.c_str())<(int)n;
}
extern "C" bool rt_dev_mode() { return false; }
extern "C" uint64_t plat_now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
extern "C" void plat_sleep_until_ns(uint64_t t) { auto now=plat_now_ns(); if(t>now)std::this_thread::sleep_for(std::chrono::nanoseconds(t-now)); }
extern "C" intptr_t plat_spawn(const char *const *a,const char *l,bool append) { return start_cb(a,l,append); }
extern "C" int plat_proc_wait(intptr_t p,bool block) { return wait_cb(p,block); }
extern "C" void plat_proc_kill(intptr_t p) { kill_cb(p); }
// (ZA Recomp's gamebuild.c also logs to launcher.log, asks for free memory after a failed compile and for the CPU level)
#include <cstdarg>
extern "C" void rt_launcher_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
extern "C" void plat_mem_free(uint64_t *phys, uint64_t *commit) { *phys = *commit = 0; }
extern "C" bool gamelib_old_cpu(void) { return false; }
'''
GENERATOR = r'''
import pathlib,sys,time
name,source,base,out=sys.argv[1:5]
out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
for i in range(3):
    (out/f'{name}_{i:03}.c').write_text(f'int {name}_{i:03}(void) {{ return {i}; }}\n')
    print(f'{name}: progress {i+1}/3 files {i+1}',file=sys.stderr,flush=True)
    time.sleep(.03)
(out/f'{name}_table.c').write_text(f'int {name}_table(void) {{ return 3; }}\n')
'''

with tempfile.TemporaryDirectory() as temporary:
    tmp = Path(temporary).resolve()
    resources, game = tmp/'resources', tmp/'game'
    (resources/'runtime/hle').mkdir(parents=True)
    (resources/'tools').mkdir()
    (resources/'build').mkdir()
    (game/'exefs').mkdir(parents=True)
    for name in ('cpu.h','simd.h','simd_fp.h','gamelib.h'):
        (resources/'runtime'/name).write_text('/* fixture header */\n')
    (resources/'tools/gamelib_entry.c').write_text('int cache_entry(void) { return 0; }\n')
    (resources/'runtime/hle/hle_symbols.txt').write_text('fixture\n')
    main = b'NSO0' + bytes(0x3c) + bytes.fromhex('0123456789abcdef') + bytes(0x18) + b'fixture main'  # (the build ID: the library id)
    (game/'exefs/main').write_bytes(main)
    (game/'exefs/sdk').write_bytes(b'fixture sdk')
    generator = tmp/'generator.py'; generator.write_text(GENERATOR)
    cc = shlex.split(os.environ.get('CC','clang'))
    cxx = shlex.split(os.environ.get('CXX','clang++'))
    compiler = shutil.which(cc[0]); assert compiler
    relocated = tmp/'relocated'/Path(compiler).name
    # The native helper receives its normal argv; the process bridge invokes the fixture script.
    os.environ['LARECOMP_GEN'] = str(generator)
    os.environ['LARECOMP_CC'] = compiler
    if os.name == 'nt':
        stub=tmp/'stub.c'; stub.write_text('int fixture_export;\n')
        subprocess.run(cc+[str(stub),'-shared','-Wl,--out-implib,'+str(resources/'build/lasrecomp.dll.a'),
                           '-o',str(tmp/'stub.dll')],check=True)
    objects=[]
    for source in ('runtime/gamebuild.c',):
        obj=tmp/(Path(source).stem+'.o'); objects.append(str(obj))
        subprocess.run(cc+['-std=gnu11','-fPIC','-Iruntime','-DGAMELIB_ID="cache-fixture"',
                           '-DGAMELIB_CFLAGS="-O0 -fPIC"','-DGAMELIB_SHARED="-shared"',
                           '-c',source,'-o',str(obj)],cwd=ROOT,check=True)
    bridge=tmp/'bridge.cpp'; bridge.write_text(BRIDGE)
    libfile=tmp/('scheduler.dll' if os.name=='nt' else 'scheduler.so')
    subprocess.run(cxx+['-shared','-fPIC','-pthread','-std=c++17','-Iruntime','-x','c++',str(bridge),
                        'runtime/build_cache.cpp','-x','none',*objects,'-o',str(libfile)]+
                   (['-Wl,--export-all-symbols'] if os.name=='nt' else []),cwd=ROOT,check=True)
    dll_search = os.add_dll_directory(str(Path(compiler).parent)) if os.name == 'nt' else None
    library=ct.CDLL(str(libfile))
    SPAWN=ct.CFUNCTYPE(ct.c_ssize_t,ct.POINTER(ct.c_char_p),ct.c_char_p,ct.c_bool)
    WAIT=ct.CFUNCTYPE(ct.c_int,ct.c_ssize_t,ct.c_bool)
    KILL=ct.CFUNCTYPE(None,ct.c_ssize_t)
    PROGRESS=ct.CFUNCTYPE(None,ct.c_double,ct.c_char_p)
    CANCEL=ct.CFUNCTYPE(ct.c_bool)
    class Options(ct.Structure):
        _fields_=[('progress',PROGRESS),('jobs',ct.c_int),('cancelled',CANCEL)]
    processes={}; launched=[]; next_id=0; fail_compile=False; fail_link=False
    @SPAWN
    def spawn(args, log, append):
        global next_id, fail_compile, fail_link
        command=[];i=0
        while args[i]: command.append(args[i].decode()); i+=1
        launched.append(command.copy())
        # The relocated compiler has identical bytes; invoke the installed copy with its platform resources.
        if command[0] == str(relocated): command[0] = compiler
        if command[0]==str(generator): command=[sys.executable,*command]
        elif (fail_compile and '-c' in command) or (fail_link and '-c' not in command):
            # (a compile fails every try: ZA Recomp's gamebuild.c tries a failed file twice more - clang crashing
            # under memory pressure, #116 - so a one-shot failure would just be retried)
            if '-c' in command: fail_compile-=1
            else: fail_link=False
            command=[sys.executable,'-c','import sys;print("synthetic failure");sys.exit(7)']
        stream=open(log.decode(),'ab' if append else 'wb')
        try:
            p=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0,
                start_new_session=os.name!='nt')
        finally: stream.close()
        next_id+=1; processes[next_id]=p; return next_id
    @WAIT
    def wait(pid,block):
        p=processes[pid]; result=p.wait() if block else p.poll()
        if result is None:return -1
        del processes[pid];return result
    @KILL
    def kill(pid):
        p=processes.pop(pid)
        if p.poll() is None:
            if os.name=='nt': subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            else: os.killpg(p.pid,signal.SIGKILL)
        p.wait()
    @PROGRESS
    def progress(fraction,step): pass
    cancel_enabled=True
    work=game/'.gamebuild'
    @CANCEL
    def cancelled():
        return (cancel_enabled and (work/'main.generation').exists() and (work/'sdk.generation').exists()
                and len(list(work.glob('*.receipt')))>=2)
    options=Options(progress,1,cancelled)
    library.test_init.argtypes=[ct.c_char_p,SPAWN,WAIT,KILL]
    library.test_init(str(resources).encode(),spawn,wait,kill)
    library.gamebuild_run.argtypes=[ct.c_char_p,ct.POINTER(Options),ct.c_void_p,ct.c_size_t]
    library.gamebuild_run.restype=ct.c_bool
    why=ct.create_string_buffer(1024)
    def run():
        launched.clear();why.value=b''
        return library.gamebuild_run(str(game).encode(),ct.byref(options),why,len(why))
    def compiles(): return [a for a in launched if '-c' in a]
    def generators(): return [a for a in launched if a[0]==str(generator)]
    try:
        assert not run() and b'cancelled' in why.value,why.value
        assert (work/'in-progress').exists()
        saved=len(list(work.glob('*.receipt'))); assert 0<saved<9
        cancel_enabled=False
        assert run(),why.value
        assert not generators() and len(compiles())==10-saved
        assert not (work/'main').exists() and not (work/'sdk').exists() and not (work/'in-progress').exists()
        output=next(p for p in game.glob('lasgame.*') if p.suffix!='.id')
        before=hashlib.sha256(output.read_bytes()).digest()
        (work/'main_000.o').write_bytes(b'corrupt')
        assert run(),why.value
        assert len(compiles())==2 and len(generators())==2
        (work/'main').mkdir()
        (work/'main/main_000.c').write_text('corrupt source\n')
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==1
        before=hashlib.sha256(output.read_bytes()).digest() # PE link timestamps may differ after a successful relink
        fail_link=True
        assert not run() and b'link' in why.value,why.value
        assert not (work/'in-progress').exists()
        assert hashlib.sha256(output.read_bytes()).digest()==before
        (work/'sdk_000.o').write_bytes(b'corrupt')
        fail_compile=3
        assert not run(),why.value
        fail_compile=0
        assert hashlib.sha256(output.read_bytes()).digest()==before
        assert run(),why.value
        assert len(compiles())==2 and not generators()
        (game/'exefs/main').write_bytes(main+b'changed input')
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==1
        relocated.parent.mkdir()
        shutil.copy(compiler, relocated) # contents/mode only; macOS system file flags are not part of the identity
        os.environ['LARECOMP_CC'] = str(relocated)
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==1 # moving the same binary does not recompile every object
        (resources/'runtime/cpu.h').write_text('/* changed header */\n')
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==9
        print('Build scheduler: cancellation/resume, cleanup, repair, failed-build preservation, compiler relocation and invalidation passed')
    finally:
        for pid in list(processes): kill(pid)
        if os.name=='nt':
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
            dll_search.close()

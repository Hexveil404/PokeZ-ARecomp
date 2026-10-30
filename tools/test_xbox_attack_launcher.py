"""Exercise the real launcher scheduling/argv on a scratch filesystem, without ImGui/game."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'runtime/ui/launcher.mm').read_text(encoding='utf-8')
start=source.index('#define PROMPTS_VERSION')
stop=source.index('// progress (the overlay',start)
code=r'''
#include <assert.h>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#endif
#include "runtime/xbox_attack_layout.h"
#define PLAT_EXE_SUFFIX ".exe"
typedef int PlatProc;
static std::string g_root;
static int16_t g_bind_pad[16],g_bind_key[24];
static struct {bool xbox_attack_layout;} g_settings;
static void settings_load(void) {}
static unsigned spawned,waited,killed;
static bool available=true;
static std::vector<std::string> last_args;
static bool file_exists(const std::string &p) {struct stat s;return !stat(p.c_str(),&s);}
static bool rt_resource(const char *,const char *,char *p,size_t n) {snprintf(p,n,"packaged-helper.exe");return available;}
static int plat_spawn(const char *const *a,const char *,bool) {
    last_args.clear();for(int i=0;a[i];i++)last_args.push_back(a[i]);return ++spawned;
}
static int plat_proc_wait(int,bool wait) {if(wait)waited++;return wait?0:-1;}
static void plat_proc_kill(int) {killed++;}
''' + source[start:stop] + r'''
static void write(const char *path,const std::string &data) {FILE *f=fopen(path,"wb");assert(f);fwrite(data.data(),1,data.size(),f);fclose(f);}
static void marked(const char *path,int ready) {write(path,prompts_stamp()+" "+std::to_string(ready));}
static void reset_attempts(void) {unlink("textures/prompts.tried");}
static void put64(uint8_t *p,uint64_t n) {for(int i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(void) {
    mkdir("game",0755);mkdir("game/exefs",0755);g_root="game";
    mkdir("textures",0755);mkdir("textures/prompts",0755);
    mkdir("mods",0755);mkdir(XBOX_ATTACK_BASE,0755);mkdir(XBOX_ATTACK_ROOT,0755);
    mkdir(XBOX_ATTACK_ROOT "/ui",0755);mkdir(XBOX_ATTACK_ROOT "/ui/data",0755);
    mkdir(XBOX_ATTACK_ROOT "/ui/data/btl_app",0755);
    write("game/romfs.bin","synthetic");write("game/exefs/main",std::string(128,'x'));
    available=false;assert(!prompts_wanted());available=true;
    // Existing installs may already have controller icons: still prepare Xbox.
    marked("textures/prompts/.made",1);assert(prompts_wanted());
    write(XBOX_ATTACK_STATUS,prompts_stamp());assert(prompts_wanted());
    marked(XBOX_ATTACK_STATUS,2);assert(prompts_wanted());
    // A known unsupported source is remembered, so icons are not regenerated forever.
    marked(XBOX_ATTACK_STATUS,0);assert(!prompts_wanted());
    // Positive status without assets or with corrupt metadata must be repaired.
    marked(XBOX_ATTACK_STATUS,1);assert(prompts_wanted());write(XBOX_ATTACK_METADATA,"truncated");assert(prompts_wanted());
    uint8_t meta[80]={0};memcpy(meta,"ZAXBOX01",8);
    uint64_t values[]={512,1,0,16,1,32,64,96,128};
    for(int i=0;i<9;i++)put64(meta+8+8*i,values[i]);
    write(XBOX_ATTACK_METADATA,std::string((char *)meta,sizeof meta));
    write(XBOX_ATTACK_HOST,std::string(512,'x'));assert(!prompts_wanted());
    std::string before=prompts_stamp();write("game/romfs.bin","new source with another size");
    assert(prompts_stamp()!=before && prompts_wanted());
    marked("textures/prompts/.made",1);marked(XBOX_ATTACK_STATUS,1);assert(!prompts_wanted());
    before=prompts_stamp();struct stat main_stat;assert(!stat("game/exefs/main",&main_stat));
    std::string changed_main(128,'x');changed_main[64]='y';write("game/exefs/main",changed_main);
    struct utimbuf times={main_stat.st_atime,main_stat.st_mtime};assert(!utime("game/exefs/main",&times));
    assert(prompts_stamp()!=before && prompts_wanted()); // real game version even if size/mtime were preserved
    marked("textures/prompts/.made",1);marked(XBOX_ATTACK_STATUS,1);
    before=prompts_stamp();g_bind_pad[0]=1;assert(prompts_stamp()!=before && prompts_wanted());
    marked("textures/prompts/.made",1);marked(XBOX_ATTACK_STATUS,0);assert(!prompts_wanted());
    unlink(XBOX_ATTACK_STATUS);marked("textures/prompts.tried",3);assert(!prompts_wanted());reset_attempts();
    g_settings.xbox_attack_layout=false;launcher_prompts_kick("game");assert(spawned==1 && !waited);
    bool flag=false,romfs=false;
    for(size_t i=0;i<last_args.size();i++) {
        if(last_args[i]=="--xbox-attack-layout")flag=true;
        if(last_args[i]=="--romfs" && i+1<last_args.size() && last_args[i+1]=="game/romfs.bin")romfs=true;
    }
    assert(flag && romfs);reset_attempts();g_settings.xbox_attack_layout=true;
    launcher_prompts_kick("game");assert(spawned==2 && waited==2 && !killed); // wait existing process, then missing assets
    assert(make_prompts(true) && waited==3); // normal setup waits for the shipped helper
    reset_attempts();xbox_layout_before_play();assert(spawned==4 && waited==4); // normal PLAY path, too
    puts("PASS: existing install, missing/corrupt resources, unsupported caching, game/binding updates, retry cap, automatic argv and enabled-boot waiting");
}
'''
with tempfile.TemporaryDirectory(prefix='xbox-launcher-') as td:
    tmp=Path(td);c=tmp/'test.cpp';c.write_text(code,encoding='utf-8')
    exe=tmp/('test.exe' if os.name=='nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CXX','clang++'))+['-std=c++17','-O2','-I',str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
assert 'make_prompts(true);' in source[source.index('check:\n'):source.index('done:\n',source.index('check:\n'))]
assert 'xbox_layout_before_play();' in source[source.index('// play: hand the window to the game'):]
ui=(ROOT/'runtime/ui/controls_ui.mm').read_text(encoding='utf-8')
assert 'TR("Xbox attack layout", "Layout de golpes Xbox")' in ui
spec=(ROOT/'tools/zaprompts.spec').read_text(encoding='utf-8')
dist=(ROOT/'tools/za/dist.sh').read_text(encoding='utf-8')
assert "hiddenimports=['xbox_attack_layout']" in spec and '--xbox-attack-layout-version' in dist
assert 'cp -R build/helper/zaprompts' in dist
print('PASS: setup integration, PT/EN option and release helper inclusion/gate')

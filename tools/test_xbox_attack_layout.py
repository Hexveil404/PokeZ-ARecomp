"""CPU tests of the actual runtime transform; never launches the game."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/za'))
import xbox_attack_layout as asset

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime/xbox_attack_layout.h"
int main(int argc, char **argv) {
    const unsigned slot[4]={2,0,3,1}, pos[4]={3,1,2,0};
    unsigned good=0,bad=0;
    uint8_t out[4];
    for (int a=-1;a<=4;a++) for (int b=-1;b<=4;b++)
    for (int x=-1;x<=4;x++) for (int y=-1;y<=4;y++) {
        int16_t p[4]={a,b,x,y}, saved[4];memcpy(saved,p,sizeof p);
        bool expected=true;
        for (unsigned i=0;i<4;i++) {
            if (p[i]<0 || p[i]>3) expected=false;
            for (unsigned j=0;j<i;j++) if (p[i]==p[j]) expected=false;
        }
        memset(out,0xa5,sizeof out);
        assert(xbox_attack_mapping(p,out)==expected);
        assert(!memcmp(saved,p,sizeof p));
        if (!expected) {for (unsigned i=0;i<4;i++) assert(out[i]==0xa5);bad++;continue;}
        for (unsigned i=0;i<4;i++) assert(out[pos[p[slot[i]]]]==i);
        good++;
    }
    assert(good==24 && bad==1272);
    int16_t xbox[4]={0,1,2,3}, nintendo[4]={1,0,3,2};
    const uint8_t xb[4]={2,3,0,1}, nin[4]={0,1,2,3};
    assert(xbox_attack_mapping(xbox,out) && !memcmp(out,xb,4));
    assert(xbox_attack_mapping(nintendo,out) && !memcmp(out,nin,4));
    const char *why=NULL;
    uint8_t tiny[1]={0x5a};
    assert(!xbox_attack_relabel(tiny,1,xbox,&why) && why && tiny[0]==0x5a);
    printf("PASS: 24 face permutations, 1272 rejected bindings, Xbox/Nintendo controls, short input\n");
    if (argc<2) return 0;
    FILE *f=fopen(argv[1],"rb");assert(f);
    uint8_t *original=malloc(XBOX_ATTACK_ARC_SIZE), *copy=malloc(XBOX_ATTACK_ARC_SIZE);
    assert(original && copy);
    assert(fread(original,1,XBOX_ATTACK_ARC_SIZE,f)==XBOX_ATTACK_ARC_SIZE && fgetc(f)==EOF);fclose(f);
    assert(xbox_attack_fnv(original,XBOX_ATTACK_ARC_SIZE)==XBOX_ATTACK_ARC_FNV);
    unsigned transformed=0;
    for(int a=0;a<4;a++) for(int b=0;b<4;b++) for(int x=0;x<4;x++) for(int y=0;y<4;y++) {
        int16_t p[4]={a,b,x,y}, saved[4];memcpy(saved,p,sizeof p);
        if(!xbox_attack_mapping(p,out))continue;
        memcpy(copy,original,XBOX_ATTACK_ARC_SIZE);
        assert(xbox_attack_relabel(copy,XBOX_ATTACK_ARC_SIZE,p,&why) && !why);
        assert(!memcmp(saved,p,sizeof p));
        for(size_t i=0;i<XBOX_ATTACK_ARC_SIZE;i++) {
            int pane=-1;
            for(int j=0;j<4;j++) if(i==xbox_attack_digits[j])pane=j;
            assert(copy[i]==(pane<0 ? original[i] : '0'+out[pane]));
        }
        transformed++;
    }
    assert(transformed==24);
    size_t corrupt[]={0,24,27408,XBOX_ATTACK_ARC_SIZE-1};
    for(unsigned i=0;i<sizeof corrupt/sizeof *corrupt;i++) {
        memcpy(copy,original,XBOX_ATTACK_ARC_SIZE);copy[corrupt[i]]^=1;
        uint64_t before=xbox_attack_fnv(copy,XBOX_ATTACK_ARC_SIZE);
        assert(!xbox_attack_relabel(copy,XBOX_ATTACK_ARC_SIZE,xbox,&why) && why);
        assert(xbox_attack_fnv(copy,XBOX_ATTACK_ARC_SIZE)==before);
    }
    memcpy(copy,original,XBOX_ATTACK_ARC_SIZE);
    int16_t duplicate[4]={0,0,2,3};
    assert(!xbox_attack_relabel(copy,XBOX_ATTACK_ARC_SIZE,duplicate,&why));
    assert(!memcmp(copy,original,XBOX_ATTACK_ARC_SIZE));
    assert(!xbox_attack_relabel(copy,XBOX_ATTACK_ARC_SIZE-1,xbox,&why));
    assert(!memcmp(copy,original,XBOX_ATTACK_ARC_SIZE));
    free(copy);free(original);
    printf("PASS: 24 real-SARC transforms, every other byte preserved, corrupted/invalid inputs unchanged\n");
}
'''

SETTINGS_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings.h"
bool bind_settings_load(const char *k,const char *v) {(void)k;(void)v;return false;}
void bind_settings_save(FILE *f) {(void)f;}
int main(int argc,char **argv) {
    assert(argc==3 && !g_settings.xbox_attack_layout);
    settings_load();
    assert(g_settings.xbox_attack_layout==(atoi(argv[1])!=0));
    assert(g_settings.battle_abxy_position==(atoi(argv[2])!=0));
    settings_save();
}
'''

def test_settings(cc,tmp):
    cfile=tmp/'settings.c';cfile.write_text(SETTINGS_HARNESS,encoding='utf-8')
    exe=tmp/('settings.exe' if os.name=='nt' else 'settings')
    subprocess.run(cc+['-std=gnu11','-I',str(ROOT/'runtime'),str(ROOT/'runtime/settings.c'),
                      str(ROOT/'runtime/cosmetic_colour.c'),str(cfile),'-o',str(exe),'-lm'],check=True)
    for nl in ['\n','\r\n']:
        for value in [None,0,1]:
            for bypos in [0,1]:
                ini=tmp/'private-test.ini'
                lines=[f'battle_abxy_position = {bypos}']
                if value is not None:lines.append(f'xbox_attack_layout = {value}')
                ini.write_bytes((nl.join(lines)+nl).encode())
                subprocess.run([str(exe),str(value or 0),str(bypos)],
                               env=dict(os.environ,LARECOMP_SETTINGS=str(ini)),check=True)
                saved=ini.read_text()
                assert f'xbox_attack_layout = {value or 0}\n' in saved
                assert f'battle_abxy_position = {bypos}\n' in saved
    print('PASS: 12 real settings-parser/default/roundtrip combinations; existing input option preserved')

def test_layer_prepare(cc,tmp,resource,installation):
    fs=(ROOT/'runtime/services/fs.c').read_text(encoding='utf-8')
    start=fs.index('static bool xbox_layer_prepare(void) {')
    end=fs.index('\nstatic int name_cmp',start)
    function=fs[start:end]
    image=installation/'extracted/romfs.bin'
    with image.open('rb') as f:
        h=struct.unpack('<10Q',f.read(80));f.seek(h[7]);meta=f.read(h[8])
    p=0;packs=[]
    while p+32<=len(meta):
        parent,sibling,offset,size,hh,n=struct.unpack_from('<IIQQII',meta,p)
        if meta[p+32:p+32+n]==b'data.trpfs':packs.append((h[9]+offset,size))
        p+=32+((n+3)&~3)
    assert len(packs)==1
    harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "runtime/settings.h"
#include "runtime/input_bind.h"
#include "runtime/xbox_attack_layout.h"
Settings g_settings;
int16_t g_bind_pad[BIND_PAD_COUNT]={0,1,2,3};
typedef struct {uint64_t off,size;const char *path;uint32_t meta;} RomFile;
static RomFile rom_files[1];static int rom_nfiles=1;
static uint8_t *xbox_arc;
static FILE *image;static unsigned reads;static bool corrupt;
static ssize_t base_pread(void *dst,uint64_t n,uint64_t off) {
    reads++;assert(!fseeko(image,(off_t)off,SEEK_SET));
    size_t got=fread(dst,1,(size_t)n,image);
    if(corrupt && got)((uint8_t *)dst)[0]^=1;
    return (ssize_t)got;
}
#define LOG(...) ((void)0)
''' + function + r'''
int main(int argc,char **argv) {
    assert(argc==4);image=fopen(argv[1],"rb");assert(image);
    rom_files[0]=(RomFile){strtoull(argv[2],NULL,10),strtoull(argv[3],NULL,10),"arc/data.trpfs",0};
    int16_t saved[4];memcpy(saved,g_bind_pad,sizeof saved);
    assert(!xbox_layer_prepare() && !reads && !xbox_arc); // default OFF
    g_settings.xbox_attack_layout=true;g_settings.battle_abxy_position=true;
    assert(!xbox_layer_prepare() && !reads && !xbox_arc);
    g_settings.battle_abxy_position=false;g_bind_pad[1]=0;
    assert(!xbox_layer_prepare() && !reads && !xbox_arc);
    g_bind_pad[1]=1;uint64_t size=rom_files[0].size;rom_files[0].size=1;
    assert(!xbox_layer_prepare() && !reads && !xbox_arc);rom_files[0].size=size;
    corrupt=true;assert(!xbox_layer_prepare() && reads==1 && !xbox_arc);corrupt=false;
    // The private fixture is already placed at the actual hidden mod path.
    FILE *f=fopen(XBOX_ATTACK_HOST,"rb+");assert(f);int byte=fgetc(f);
    rewind(f);fputc(byte^1,f);fclose(f);
    assert(!xbox_layer_prepare() && !xbox_arc);
    f=fopen(XBOX_ATTACK_HOST,"rb+");assert(f);fputc(byte,f);fclose(f);
    assert(xbox_layer_prepare() && xbox_arc);
    const uint8_t want[4]={2,3,0,1};
    for(int i=0;i<4;i++)assert(xbox_arc[xbox_attack_digits[i]]=='0'+want[i]);
    assert(!memcmp(saved,g_bind_pad,sizeof saved));
    free(xbox_arc);xbox_arc=NULL;fclose(image);
    puts("PASS: actual FS preparation: default/conflict/binding/range/version/asset rejection, valid source and unchanged input");
}
'''
    file=tmp/'fs-prepare.c';file.write_text(harness,encoding='utf-8')
    exe=tmp/('fs-prepare.exe' if os.name=='nt' else 'fs-prepare')
    subprocess.run(cc+['-std=gnu11','-D_FILE_OFFSET_BITS=64','-O2','-Wall','-Wno-unused-function','-I',str(ROOT),
                      str(file),'-o',str(exe)],check=True)
    fixture=tmp/'mods/_XboxAttackLayout/romfs'/asset.VIRTUAL
    fixture.parent.mkdir(parents=True);shutil.copyfile(resource,fixture)
    subprocess.run([str(exe),str(image.resolve()),str(packs[0][0]),str(packs[0][1])],cwd=tmp,check=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--installation',type=Path,help='optional read-only game installation for filesystem tests')
    args=ap.parse_args()
    if args.installation and not (args.installation/'extracted/romfs.bin').is_file():
        ap.error('installation must contain extracted/romfs.bin')
    if args.installation and not (ROOT/'assets/xbox_attack_layout/romfs'/asset.VIRTUAL).is_file():
        ap.error('prepare the local resource with tools/za/xbox_attack_layout.py --prepare first')
    cc=shlex.split(os.environ.get('CC','clang'))
    resource=ROOT/'assets/xbox_attack_layout/romfs'/asset.VIRTUAL
    with tempfile.TemporaryDirectory(prefix='xbox-layout-cpu-') as tmp:
        source=Path(tmp)/'test.c';source.write_text(HARNESS,encoding='utf-8')
        exe=Path(tmp)/('test.exe' if os.name=='nt' else 'test')
        subprocess.run(cc+['-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT),str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)]+([str(resource)] if resource.exists() else []),check=True)
        test_settings(cc,Path(tmp))
        if args.installation:
            test_layer_prepare(cc,Path(tmp),resource,args.installation)
        else:
            print('SKIP: filesystem preparation tests (pass --installation after local resource preparation)')
    if resource.exists():
        data=resource.read_bytes();info=asset.inspect(data)
        recorded=json.loads((resource.parents[4]/'resource.json').read_text(encoding='utf-8'))
        for key,value in info.items():
            # JSON represents geometry tuples as arrays.
            assert json.loads(json.dumps(value))==recorded[key],key
        mem={name:content for name,_,content in asset.members(data)}
        layout=mem[asset.MEMBER]
        for i in range(4): assert layout.count(f'L_waza_{i:02d}'.encode())==1
        for name,content in mem.items():
            if name.endswith('.bflan'): assert b'L_waza_' not in content,name
        print('PASS: actual FLYT/SARC identities, geometry, hash and animation-reference preconditions')
    else: print('SKIP: private battle asset tests (prepare from your own game)')
    fs=(ROOT/'runtime/services/fs.c').read_text(encoding='utf-8')
    assert 'if (!g_settings.xbox_attack_layout) return false;' in fs
    assert 'if (g_settings.battle_abxy_position)' in fs
    assert 'source resource/version mismatch' in fs
    assert 'xbox_attack_fnv(packed, XBOX_ATTACK_PACK_SIZE) == XBOX_ATTACK_PACK_FNV' in fs
    assert 'strcmp(ov[i].path, XBOX_ATTACK_HOST)' in fs
    assert 'e->d_name[0] == \'_\'' in fs
    settings=(ROOT/'runtime/settings.c').read_text(encoding='utf-8')
    assert '"xbox_attack_layout"' in settings and 'xbox_attack_layout = %d' in settings
    print('PASS: opt-in, source-version guard, isolated overlay and setting integration checks')

if __name__=='__main__':main()

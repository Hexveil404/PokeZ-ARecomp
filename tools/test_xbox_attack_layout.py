"""CPU tests of the production mapping, runtime metadata, filesystem and settings."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/za'))
import xbox_attack_layout as asset

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime/xbox_attack_layout.h"
static void put64(uint8_t *p,uint64_t v) { for(int i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8)); }
int main(int argc,char **argv) {
    assert(argc==3);
    uint8_t meta[80];FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(meta,1,80,f)==80 && fgetc(f)==EOF);fclose(f);
    XboxAttackResource xr;assert(xbox_attack_resource(meta,80,&xr));
    for(size_t n=0;n<80;n++)assert(!xbox_attack_resource(meta,n,&xr));
    uint8_t bad[80];memcpy(bad,meta,80);bad[0]^=1;assert(!xbox_attack_resource(bad,80,&xr));
    const int fields[]={0,3,5,6,7,8};
    for(unsigned i=0;i<sizeof fields/sizeof *fields;i++) {
        memcpy(bad,meta,80);put64(bad+8+8*fields[i],UINT64_MAX);
        assert(!xbox_attack_resource(bad,80,&xr));
    }
    memcpy(bad,meta,80);put64(bad+8+8*2,UINT64_MAX);
    assert(!xbox_attack_resource(bad,80,&xr));
    memcpy(bad,meta,80);memcpy(bad+8+8*6,bad+8+8*5,8);
    assert(!xbox_attack_resource(bad,80,&xr));
    assert(xbox_attack_resource(meta,80,&xr));
    for(int field=0;field<=3;field+=3) {
        memcpy(bad,meta,80);put64(bad+8+8*field,0);
        assert(!xbox_attack_resource(bad,80,&xr));
    }
    uint8_t *original=malloc(xr.arc_size),*copy=malloc(xr.arc_size);assert(original && copy);
    f=fopen(argv[2],"rb");assert(f);
    assert(fread(original,1,xr.arc_size,f)==xr.arc_size && fgetc(f)==EOF);fclose(f);
    assert(xbox_attack_fnv(original,xr.arc_size)==xr.arc_fnv);
    const unsigned slot[4]={2,0,3,1},pos[4]={3,1,2,0};
    unsigned good=0,bad_bind=0;uint8_t out[4];const char *why;
    for(int a=-1;a<=4;a++)for(int b=-1;b<=4;b++)for(int x=-1;x<=4;x++)for(int y=-1;y<=4;y++) {
        int16_t p[4]={a,b,x,y},saved[4];memcpy(saved,p,sizeof p);bool want=true;
        for(unsigned i=0;i<4;i++) {
            if(p[i]<0 || p[i]>3)want=false;
            for(unsigned j=0;j<i;j++)if(p[i]==p[j])want=false;
        }
        memset(out,0xa5,4);assert(xbox_attack_mapping(p,out)==want);assert(!memcmp(p,saved,sizeof p));
        memcpy(copy,original,xr.arc_size);
        assert(xbox_attack_relabel(copy,xr.arc_size,p,&xr,&why)==want);
        if(!want) {
            for(int i=0;i<4;i++)assert(out[i]==0xa5);
            assert(why && !memcmp(copy,original,xr.arc_size));bad_bind++;continue;
        }
        assert(!why);for(unsigned i=0;i<4;i++)assert(out[pos[p[slot[i]]]]==i);
        for(size_t i=0;i<xr.arc_size;i++) {
            int pane=-1;for(int j=0;j<4;j++)if(i==xr.digits[j])pane=j;
            assert(copy[i]==(pane<0 ? original[i] : '0'+out[pane]));
        }
        good++;
    }
    assert(good==24 && bad_bind==1272);
    int16_t xbox[4]={0,1,2,3},nin[4]={1,0,3,2};
    const uint8_t xb[4]={2,3,0,1},nv[4]={0,1,2,3};
    assert(xbox_attack_mapping(xbox,out) && !memcmp(out,xb,4));
    assert(xbox_attack_mapping(nin,out) && !memcmp(out,nv,4));
    const size_t corrupt[]={0,24,xr.digits[0],xr.arc_size-1};
    for(unsigned i=0;i<sizeof corrupt/sizeof *corrupt;i++) {
        memcpy(copy,original,xr.arc_size);copy[corrupt[i]]^=1;
        uint64_t before=xbox_attack_fnv(copy,xr.arc_size);
        assert(!xbox_attack_relabel(copy,xr.arc_size,xbox,&xr,&why) && why);
        assert(xbox_attack_fnv(copy,xr.arc_size)==before);
    }
    memcpy(copy,original,xr.arc_size);
    assert(!xbox_attack_relabel(copy,xr.arc_size-1,xbox,&xr,&why));
    assert(!memcmp(copy,original,xr.arc_size));
    free(copy);free(original);
    puts("PASS: 24 permutations, 1272 invalid bindings, every other archive byte preserved, metadata bounds/overflow/schema and corrupted/short inputs");
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
    assert(g_settings.battle_abxy_position==(atoi(argv[2])!=0));settings_save();
}
'''


def compile_c(cc, tmp, name, code, extra=()):
    cfile=tmp/(name+'.c');cfile.write_text(code,encoding='utf-8')
    exe=tmp/(name+('.exe' if os.name=='nt' else ''))
    subprocess.run(cc+['-std=gnu11','-D_FILE_OFFSET_BITS=64','-O2','-Wall','-Wno-unused-function',
                      '-I',str(ROOT),'-I',str(ROOT/'runtime'),str(cfile),*map(str,extra),'-o',str(exe),'-lm'],check=True)
    return exe


def test_settings(cc,tmp):
    exe=compile_c(cc,tmp,'settings',SETTINGS_HARNESS,[ROOT/'runtime/settings.c',ROOT/'runtime/cosmetic_colour.c'])
    for nl in ['\n','\r\n']:
        for value in [None,0,1]:
            for bypos in [0,1]:
                ini=tmp/'private-test.ini';lines=[f'battle_abxy_position = {bypos}']
                if value is not None:lines.append(f'xbox_attack_layout = {value}')
                ini.write_bytes((nl.join(lines)+nl).encode())
                subprocess.run([str(exe),str(value or 0),str(bypos)],env=dict(os.environ,LARECOMP_SETTINGS=str(ini)),check=True)
                saved=ini.read_text()
                assert f'xbox_attack_layout = {value or 0}\n' in saved
                assert f'battle_abxy_position = {bypos}\n' in saved
    print('PASS: 12 settings/default/roundtrip combinations; existing input option preserved')


def test_fs(cc,tmp,mod,image,pack_offset,pack_size):
    fs=(ROOT/'runtime/services/fs.c').read_text(encoding='utf-8')
    start=fs.index('static bool xbox_layer_prepare(void) {');end=fs.index('\nstatic int name_cmp',start)
    code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "runtime/settings.h"
#include "runtime/input_bind.h"
#include "runtime/xbox_attack_layout.h"
Settings g_settings;int16_t g_bind_pad[BIND_PAD_COUNT]={0,1,2,3};
typedef struct {uint64_t off,size;const char *path;uint32_t meta;} RomFile;
static RomFile rom_files[1];static int rom_nfiles=1;
static uint8_t *xbox_arc;static XboxAttackResource xbox_resource;
static FILE *image;static unsigned reads;static bool corrupt;
static ssize_t base_pread(void *dst,uint64_t n,uint64_t off) {
    reads++;assert(!fseeko(image,(off_t)off,SEEK_SET));size_t got=fread(dst,1,n,image);
    if(corrupt && got)((uint8_t *)dst)[0]^=1;return (ssize_t)got;
}
#define LOG(...) fprintf(stderr,__VA_ARGS__)
''' + fs[start:end] + r'''
int main(int argc,char **argv) {
    assert(argc==4);image=fopen(argv[1],"rb");assert(image);
    rom_files[0]=(RomFile){strtoull(argv[2],0,10),strtoull(argv[3],0,10),"arc/data.trpfs",0};
    int16_t saved[4];memcpy(saved,g_bind_pad,sizeof saved);
    assert(!xbox_layer_prepare() && !reads);
    g_settings.xbox_attack_layout=true;g_settings.battle_abxy_position=true;
    assert(!xbox_layer_prepare() && !reads);g_settings.battle_abxy_position=false;
    g_bind_pad[1]=0;assert(!xbox_layer_prepare() && !reads);g_bind_pad[1]=1;
    uint64_t size=rom_files[0].size;rom_files[0].size=1;
    assert(!xbox_layer_prepare() && !reads);rom_files[0].size=size;
    uint64_t off=rom_files[0].off;rom_files[0].off=UINT64_MAX;
    assert(!xbox_layer_prepare() && !reads);rom_files[0].off=off;
    assert(!rename(XBOX_ATTACK_METADATA,"metadata-hold"));
    assert(!xbox_layer_prepare() && !reads && !xbox_arc);
    assert(!rename("metadata-hold",XBOX_ATTACK_METADATA));
    assert(!rename(XBOX_ATTACK_HOST,"archive-hold"));
    assert(!xbox_layer_prepare() && !xbox_arc);
    assert(!rename("archive-hold",XBOX_ATTACK_HOST));reads=0;
    corrupt=true;assert(!xbox_layer_prepare() && reads==1 && !xbox_arc);corrupt=false;
    FILE *f=fopen(XBOX_ATTACK_METADATA,"rb+");assert(f);int byte=fgetc(f);
    rewind(f);fputc(byte^1,f);fclose(f);assert(!xbox_layer_prepare() && !xbox_arc);
    f=fopen(XBOX_ATTACK_METADATA,"rb+");fputc(byte,f);fclose(f);
    f=fopen(XBOX_ATTACK_HOST,"rb+");assert(f);byte=fgetc(f);
    rewind(f);fputc(byte^1,f);fclose(f);assert(!xbox_layer_prepare() && !xbox_arc);
    f=fopen(XBOX_ATTACK_HOST,"rb+");fputc(byte,f);fclose(f);
    assert(xbox_layer_prepare() && xbox_arc);
    uint8_t want[4]={2,3,0,1};
    for(int i=0;i<4;i++)assert(xbox_arc[xbox_resource.digits[i]]=='0'+want[i]);
    assert(!memcmp(saved,g_bind_pad,sizeof saved));free(xbox_arc);fclose(image);
    puts("PASS: actual FS default/conflict/bindings/ranges, corrupt metadata/source/archive, valid automatic resource and unchanged input");
}
'''
    exe=compile_c(cc,tmp,'fs-prepare',code)
    fixture=tmp/'mods'/asset.MOD_NAME
    shutil.copytree(mod,fixture)
    subprocess.run([str(exe),str(image.resolve()),str(pack_offset),str(pack_size)],cwd=tmp,check=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepared-mod',type=Path,help='optional automatically prepared private mod to test')
    ap.add_argument('--installation',type=Path,help='read-only installation supplying the corresponding RomFS')
    args=ap.parse_args()
    if bool(args.prepared_mod)!=bool(args.installation):ap.error('provide both --prepared-mod and --installation')
    cc=shlex.split(os.environ.get('CC','clang'))
    with tempfile.TemporaryDirectory(prefix='xbox-layout-cpu-') as td:
        tmp=Path(td)
        if args.prepared_mod:
            mod=args.prepared_mod.resolve()
            image=args.installation.resolve()/'extracted/romfs.bin'
            with image.open('rb') as f:
                h=struct.unpack('<10Q',f.read(80));f.seek(h[7]);fm=f.read(h[8])
            p=0;packs=[]
            while p+32<=len(fm):
                _,_,off,size,_,n=struct.unpack_from('<IIQQII',fm,p)
                if fm[p+32:p+32+n]==b'data.trpfs':packs.append((h[9]+off,size))
                p+=32+((n+3)&~3)
            assert len(packs)==1
            base,size=packs[0]
            info=asset.inspect((mod/'romfs'/asset.VIRTUAL).read_bytes())
            assert info['digits']==list(struct.unpack('<8s9Q',(mod/'resource.bin').read_bytes())[6:])
        else:
            mod=tmp/'synthetic';arc=bytearray(512);digits=[32,64,96,128]
            for i,at in enumerate(digits):arc[at-8:at+2]=f'L_waza_{i:02d}'.encode()+b'\0'
            image=tmp/'source.bin';packed=b'abcd'+bytes(range(16));image.write_bytes(bytes(32)+packed)
            info={'digits':digits};guard={'offset':4,'size':16,'fnv64':f'{asset.fnv(packed[4:]):016x}'}
            out=mod/'romfs'/asset.VIRTUAL;out.parent.mkdir(parents=True);out.write_bytes(arc)
            (mod/'resource.bin').write_bytes(asset.metadata(arc,info,guard));base,size=32,len(packed)
        exe=compile_c(cc,tmp,'mapping',HARNESS)
        subprocess.run([str(exe),str(mod/'resource.bin'),str(mod/'romfs'/asset.VIRTUAL)],check=True)
        test_settings(cc,tmp)
        test_fs(cc,tmp,mod,image,base,size)
    print('PASS: runtime uses local metadata; no generated C header, game execution or installed config writes')


if __name__=='__main__':main()

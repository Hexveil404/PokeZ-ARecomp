"""Build/extract a component ZIP and run its real frozen setup helper, never the game.

All generated game-derived outputs stay in a temporary test installation. The
ZIP contains only the executable, helper/runtime libraries and project docs.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--helper',type=Path,default=ROOT/'build/helper/zaprompts')
    ap.add_argument('--exe',type=Path,default=ROOT/'build/lasrecomp.exe')
    ap.add_argument('--installation',type=Path,required=True,help='read-only original game input/runtime DLLs')
    ap.add_argument('--archive',type=Path,default=ROOT/'build/xbox-release-components.zip')
    args=ap.parse_args()
    assert os.name=='nt','frozen Windows helper smoke test'
    rom=args.installation.resolve()/'extracted/romfs.bin'
    original_stat=rom.stat();header=sha(ROOT/'runtime/xbox_attack_resource.h')
    helper=args.helper.resolve();assert (helper/'zaprompts.exe').is_file() and args.exe.is_file()
    args.archive.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.archive,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
        z.write(args.exe,'ZA Recomp/lasrecomp.exe')
        for p in helper.rglob('*'):
            if p.is_file():z.write(p,'ZA Recomp/zaprompts/'+p.relative_to(helper).as_posix())
        for rel in ['README.md','LICENSE','docs/XBOX-ATTACK-LAYOUT.md']:
            z.write(ROOT/rel,'ZA Recomp/'+rel)
        z.write(ROOT/'third_party/ooz/COPYING','ZA Recomp/zaprompts/COPYING-GPLv3.txt')
        # This test does not rebuild the separate importer/compiler helpers or
        # LGPL FFmpeg. Existing runtime DLLs are copied read-only for ZIP shape.
        for p in args.installation.glob('*.dll'):
            assert p.name.lower()!='lasgame.dll','game library is never packaged'
            z.write(p,'ZA Recomp/'+p.name)
    with tempfile.TemporaryDirectory(prefix='ZA Xbox release ') as td:
        temp=Path(td)
        with zipfile.ZipFile(args.archive) as z:
            assert z.testzip() is None
            for name in z.namelist():
                parts=name.split('/')
                assert parts[0]=='ZA Recomp' and parts[1] not in {
                    'extracted','mods','cache','save','assets','generated','rando'}
                assert not name.lower().endswith(('.nsp','.xci','.bfttf','romfs.bin','lasgame.dll'))
            z.extractall(temp)
        release=temp/'ZA Recomp';exe=release/'zaprompts/zaprompts.exe'
        assert sha(release/'lasrecomp.exe')==sha(args.exe)
        env=dict(os.environ,PATH=os.environ['SystemRoot']+'/System32')
        env.pop('PYTHONPATH',None);env.pop('PYTHONHOME',None)
        version=subprocess.check_output([str(exe),'--xbox-attack-layout-version'],cwd=release,env=env,text=True)
        assert version.strip()=='XboxAttackLayout schema 1'
        (release/'larecomp.ini').write_text('battle_abxy_position = 0\nbind_pad_a = south\nbind_pad_b = east\n'
                                          'bind_pad_x = west\nbind_pad_y = north\n',encoding='utf-8')
        stamp='4-xbox1-release-test'
        # Exactly the normal launcher's flags, including its relative outputs.
        normal=[str(exe),'--romfs',str(rom),'--settings','larecomp.ini','--mods','mods',
                '--out','textures/prompts','--install',stamp,'--xbox-attack-layout']
        p=subprocess.run(normal,cwd=release,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        assert p.returncode==0,p.stdout.decode(errors='replace')
        assert (release/'textures/prompts/.made').read_text()==stamp
        mod=release/'mods/_XboxAttackLayout';arc=mod/'romfs/ui/data/btl_app/btl_app_waza_00.arc'
        assert (mod/'resource.bin').stat().st_size==80
        arc_sha=sha(arc);mtime=arc.stat().st_mtime_ns
        only=[str(exe),'--xbox-attack-layout-only','--romfs',str(rom),'--mods','mods','--install',stamp]
        for _ in range(2):
            subprocess.run(only,cwd=release,env=env,check=True)
            assert sha(arc)==arc_sha and arc.stat().st_mtime_ns==mtime
        bad=bytearray((mod/'resource.bin').read_bytes());bad[0]^=1;(mod/'resource.bin').write_bytes(bad)
        subprocess.run(only,cwd=release,env=env,check=True)
        assert (mod/'resource.bin').read_bytes()[:8]==b'ZAXBOX01' and sha(arc)==arc_sha
        status=(release/'mods/.XboxAttackLayout.prepared').read_bytes()
        p=subprocess.run([str(exe),'--xbox-attack-layout-only','--romfs',str(release/'missing.bin'),
                          '--mods','mods','--install','temporary-io'],cwd=release,env=env,capture_output=True)
        assert p.returncode==1 and (release/'mods/.XboxAttackLayout.prepared').read_bytes()==status
        invalid=release/'unsupported-romfs.bin';invalid.write_bytes(bytes(80))
        p=subprocess.run([str(exe),'--xbox-attack-layout-only','--romfs',str(invalid),
                          '--mods','mods','--install','changed-source'],cwd=release,env=env,capture_output=True)
        assert p.returncode==1 and b'[xbox-layout] unavailable:' in p.stdout
        assert sha(arc)==arc_sha
        assert (release/'mods/.XboxAttackLayout.prepared').read_text()=='changed-source 0\n'
        subprocess.run(only,cwd=release,env=env,check=True)
        subprocess.run([sys.executable,'-B',str(ROOT/'tools/test_xbox_attack_layout.py'),
                        '--prepared-mod',str(mod),'--installation',str(args.installation.resolve())],check=True)
    assert sha(ROOT/'runtime/xbox_attack_resource.h')==header,'helper must never generate source headers'
    now=rom.stat();assert (now.st_size,now.st_mtime_ns)==(original_stat.st_size,original_stat.st_mtime_ns)
    print('PASS: ZIP CRC/allowlist, normal frozen setup without Python/compiler on PATH, icons + overlay, repeat/repair/unsupported/update, production CPU/FS load')
    print('Component ZIP:',args.archive,'bytes:',args.archive.stat().st_size)


if __name__=='__main__':main()

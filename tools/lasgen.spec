# PyInstaller spec: the game code generator (tools/gen.py and its modules) as a standalone onedir helper, so the start
# screen's setup can build the game code on the player's machine without Python (runtime/gamebuild.c).
# `make gen-helper` -> build/helper/lasgen/ (lasgen[.exe] + _internal/). Same command line as gen.py. Inside the
# bundle (sys._MEIPASS) tools/rando/patch_variants.json sits at rando/ (gen.py finds it via __file__).
import glob
import os
import sys

from PyInstaller.utils.hooks import collect_dynamic_libs

TOOLS = os.path.normpath(SPECPATH)

a = Analysis(
    [os.path.join(TOOLS, 'gen.py')],
    pathex=[TOOLS],
    binaries=collect_dynamic_libs('capstone'),
    datas=[(os.path.join(TOOLS, 'rando', 'patch_variants.json'), 'rando'),
           (os.path.join(TOOLS, '..', 'runtime', 'game_addresses', 'versions.h'), 'runtime/game_addresses')]
          + [(p, 'runtime/game_addresses')  # (every game version's patch table: gen.py picks one by build ID)
             for p in sorted(glob.glob(os.path.join(TOOLS, '..', 'runtime', 'game_addresses', 'patches_v*.h')))],
    hiddenimports=['patches', 'lasgen_uc'],  # (gen.py imports patches once the version is known; lasgen_uc replaces unicorn, from the runtime hook)
    runtime_hooks=[os.path.join(TOOLS, 'lasgen_utf8.py')],
    excludes=['unicorn', 'tkinter', 'pydoc', 'doctest', 'unittest', 'pdb', 'IPython', 'numpy', 'setuptools', '_distutils_hack',
              'pkg_resources', 'ssl', '_ssl', '_hashlib', '_multibytecodec', '_codecs_cn', '_codecs_hk',
              '_codecs_iso2022', '_codecs_jp', '_codecs_kr', '_codecs_tw', '_decimal'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [('hash_seed=0', None, 'OPTION')],  # (the same output every run)
    exclude_binaries=True,
    name='lasgen',
    console=True,  # stderr = the progress lines the runtime reads (no window: CREATE_NO_WINDOW)
    strip=sys.platform == 'darwin',
    upx=False,
)
coll = COLLECT(exe, a.binaries, a.datas, strip=sys.platform == 'darwin', upx=False, name='lasgen')

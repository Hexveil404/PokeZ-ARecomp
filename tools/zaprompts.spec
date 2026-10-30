# PyInstaller spec: the button icon maker (tools/za/prompt_textures.py) as a standalone onedir helper, so the start
# screen can redraw Pokémon Legends Z-A's button icons for the keyboard, Xbox and PlayStation from the player's own game
# files (runtime/ui/launcher.mm make_prompts) without Python. -> build/helper/zaprompts/ (zaprompts[.exe] + _internal/),
# with ooz (third_party/ooz, GPLv3: see third_party/ooz/README.ZARecomp.md) next to the exe, built by tools/za/dist.sh
# into build/ooz/. Same command line as prompt_textures.py.
import os
import sys

from PyInstaller.utils.hooks import collect_data_files

TOOLS = os.path.normpath(SPECPATH)
ROOT = os.path.dirname(TOOLS)
OOZ = os.path.join(ROOT, 'build', 'ooz', 'ooz.dll' if sys.platform == 'win32' else 'libooz.so')

a = Analysis(
    [os.path.join(TOOLS, 'za', 'prompt_textures.py')],
    pathex=[os.path.join(TOOLS, 'za')],
    hiddenimports=['xbox_attack_layout'],  # setup's private battle overlay; no source files needed on the player's PC
    binaries=[(OOZ, '.')],  # (_internal/: prompt_textures.py looks there, sys._MEIPASS)
    datas=collect_data_files('cffsubr', include_py_files=False),  # (its tx program, which subroutinizes the fonts)
    excludes=['tkinter', 'pydoc', 'doctest', 'unittest', 'pdb', 'IPython', 'setuptools', '_distutils_hack',
              'pkg_resources', 'ssl', '_ssl', '_hashlib'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    exclude_binaries=True,
    name='zaprompts',
    console=True,  # (its lines go to the start screen's log; no window: CREATE_NO_WINDOW)
    strip=sys.platform == 'darwin',
    upx=False,
)
coll = COLLECT(exe, a.binaries, a.datas, strip=sys.platform == 'darwin', upx=False, name='zaprompts')

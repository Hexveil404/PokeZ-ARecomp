# PyInstaller runtime hook for lasgen (tools/lasgen.spec): the log is redirected to a file, so UTF-8 whatever the system
# code page (cp932 can't write "Pokémon"). Here, not in gen.py: gen.py is part of the game code's id (Makefile GAMELIB_ID).
import sys

sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')

# a native crash (unicorn, a Python extension) writes where it was: otherwise its log is empty (#99)
import faulthandler
faulthandler.enable()

#!/bin/bash
# Records a play so it can be replayed: docs/dev-tools.md.
#   EXE=<lasrecomp.exe> GAME=<game files> [LIB=<lasgame.dll>] [SAVE=<folder with save_0>] [WORK=<folder>] \
#     bash tools/za/replay-record.sh <name>
# Writes <WORK>/replays/<name>/: the save as it was, the settings, the mods list, the clock, the library, input.txt.
# Runs from a copy of the save: yours isn't touched unless SAVE_IN_PLACE=1.
N=${1:?usage: replay-record.sh <name>}
: "${EXE:?set EXE to lasrecomp.exe}" "${GAME:?set GAME to your game files (exefs + romfs.bin)}"
WORK=${WORK:-$PWD}
P=$WORK/replays/$N
[ -e "$P" ] && { echo "$P already exists"; exit 1; }
mkdir -p "$P"
cd "$WORK" || exit 1
export LARECOMP_DATA_ROOT="$(cygpath -w "$WORK" 2>/dev/null || echo "$WORK")"  # (logs, caches and screenshots here, not next to the exe)
SRC=${SAVE:-save}
[ -d "$SRC" ] || { echo "no save at $SRC (set SAVE)"; rm -rf "$P"; exit 1; }
cp -r "$SRC" "$P/save"
[ -f larecomp.ini ] && cp larecomp.ini "$P/larecomp.ini"
ls mods 2>/dev/null > "$P/mods.txt"
date +%s > "$P/clock.txt"
echo "${LIB:-}" > "$P/gamelib.txt"
export LARECOMP_RECORD="$P/input.txt" LARECOMP_CLOCK_START=$(cat "$P/clock.txt")
[ -n "$LIB" ] && export LARECOMP_GAME_LIB=$LIB
if [ -n "$SAVE_IN_PLACE" ]; then export LARECOMP_SAVE_DIR="$SRC"
else mkdir -p "$P/run" && cp -r "$P/save" "$P/run/save" && export LARECOMP_SAVE_DIR="$P/run/save"; fi
echo "recording $N: play, then close the game"
"$EXE" "$GAME" --play > "$P/record.log" 2>&1
rm -rf "$P/run"
echo "done: $(wc -l < "$P/input.txt") input changes -> $P"

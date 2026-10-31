#!/bin/bash
# Replays a recording (replay-record.sh) from its own copy of the save: docs/dev-tools.md.
#   EXE=<lasrecomp.exe> GAME=<game files> [WORK=<folder>] bash tools/za/replay-play.sh <name> [seconds]
# Env: LIB=<lasgame.dll> (default: the one recorded), SHOT=<every N frames> (screenshots in replays/<name>/run/),
# SAMPLE=<delay,dur> (profile), plus any LARECOMP_* passed through. Output: replays/<name>/run/replay.log.
N=${1:?usage: replay-play.sh <name> [seconds]}
T=${2:-0}
: "${EXE:?set EXE to lasrecomp.exe}" "${GAME:?set GAME to your game files (exefs + romfs.bin)}"
WORK=${WORK:-$PWD}
P=$WORK/replays/$N
[ -f "$P/input.txt" ] || { echo "no recording $P"; exit 1; }
cd "$WORK" || exit 1
export LARECOMP_DATA_ROOT="$(cygpath -w "$WORK" 2>/dev/null || echo "$WORK")"  # (logs, caches and screenshots here, not next to the exe)
O=$P/run
rm -rf "$O"; [ -e "$O" ] && { echo "can't clear $O (a shell or program inside it?)"; exit 1; }
mkdir -p "$O" build
cp -r "$P/save" "$O/save"
[ -f "$P/larecomp.ini" ] && cp "$P/larecomp.ini" "$O/larecomp.ini"
[ "$(ls mods 2>/dev/null)" = "$(cat "$P/mods.txt")" ] || echo "warning: mods differ from the recording"
GL=${LIB:-$(cat "$P/gamelib.txt")}
[ -n "$GL" ] && export LARECOMP_GAME_LIB=$GL
export LARECOMP_REPLAY="$P/input.txt" LARECOMP_CLOCK_START=$(cat "$P/clock.txt")
export LARECOMP_SAVE_DIR="$O/save" LARECOMP_SETTINGS="$O/larecomp.ini"
rm -f build/frame_*.png
[ -n "$SHOT" ] && export LARECOMP_SCREENSHOT_EVERY=$SHOT
[ -n "$SAMPLE" ] && export LARECOMP_SAMPLE=$SAMPLE && rm -f build/samples.txt
"$EXE" "$GAME" --play > "$O/replay.log" 2>&1 &
pid=$!
WP=$(cat /proc/$pid/winpid 2>/dev/null)
if [ "$T" -gt 0 ]; then
  for i in $(seq "$T"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
  if kill -0 $pid 2>/dev/null && [ -n "$WP" ]; then
    # (the game can be a child of the process started here: stop it by its own id, never by name)
    G=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter \"ParentProcessId=$WP\").ProcessId" | tr -d '\r')
    for k in $G $WP; do taskkill //F //PID $k > /dev/null 2>&1; done
  fi
fi
wait $pid 2>/dev/null
mv build/frame_*.png "$O/" 2>/dev/null
[ -n "$SAMPLE" ] && [ -f build/samples.txt ] && cp build/samples.txt "$O/samples.txt"
grep -a "\[replay\]" "$O/replay.log"
grep -a "^\[vk\] t=" "$O/replay.log" | awk '{f=$5+0; s+=f; n++; if(n==1||f<m)m=f} END {if(n) printf "fps: avg %.1f, min %.1f over %d samples\n", s/n, m, n}'
grep -ac "exception" "$O/replay.log" | sed 's/^/crashes: /'

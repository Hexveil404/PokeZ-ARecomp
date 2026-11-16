#!/bin/bash
# Replays a player's bug report - the zip from the start screen's "Record for a bug report" (runtime/bugrec.c) - up to
# its first mark (F8), then the controls are yours: docs/dev-tools.md.
#   EXE=<lasrecomp.exe> GAME=<game files> [MARK=<n>|none] [WORK=<folder>] bash tools/za/bug-replay.sh <zip> [seconds]
# Unpacks into <WORK>/replays/<zip name>/ (a replay-record.sh recording's layout, a ZA DevTools repro's too) and runs
# replay-play.sh there. The zip must come from this EXE's version (LARECOMP_REPLAY_ANY_VERSION=1 tries anyway).
Z=${1:?usage: bug-replay.sh <zip> [seconds]}
T=${2:-0}
: "${EXE:?set EXE to lasrecomp.exe}" "${GAME:?set GAME to your game files (exefs + romfs.bin)}"
mkdir -p "${WORK:-.}"
export WORK=$(cd "${WORK:-.}" && pwd)  # (replay-play.sh runs from WORK: relative paths made absolute first)
export EXE="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"
[ -e "$GAME" ] && export GAME="$(cd "$GAME" && pwd)"
N=$(basename "$Z" .zip)
P=$WORK/replays/$N
[ -e "$P" ] && { echo "$P already exists (remove it to unpack again)"; exit 1; }
mkdir -p "$P" && unzip -q "$Z" -d "$P" || { echo "can't unpack $Z"; exit 1; }
mkdir -p "$P/save"  # (no save when it was recorded: a new game; the zip has no empty folders)
[ -f "$P/mods.txt" ] || : > "$P/mods.txt"
grep -h "^version\|^game\|^started" "$P/info.txt" 2>/dev/null | tr '\n' ' '; echo
M=${MARK:-1}
unset LARECOMP_REPLAY_UNTIL
if [ "$M" != none ] && [ -f "$P/marks.txt" ]; then
    K=$(sed -n "${M}p" "$P/marks.txt" | sed -n 's/.* key \([0-9]*\).*/\1/p')
    [ -n "$K" ] || { echo "no mark $M in $P/marks.txt"; exit 1; }
    export LARECOMP_REPLAY_UNTIL=$K
    echo "replaying to mark $M (game time $K), then the controls are yours"
else
    echo "no mark: replaying to the end"
fi
exec bash "$(dirname "$0")/replay-play.sh" "$N" "$T"

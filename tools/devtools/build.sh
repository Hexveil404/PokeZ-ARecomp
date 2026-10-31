#!/bin/bash
# Builds zadevtools.exe (Windows, MSYS2 UCRT64: clang, SDL3, libpng) with the repository's Dear ImGui.
# The exe and the DLLs it needs land in build/devtools/. Usage: bash tools/devtools/build.sh
set -e
D=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$D/../.." && pwd)
I=$ROOT/third_party/imgui
O=$ROOT/build/devtools
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p "$O/obj"
for f in imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_sdl3 backends/imgui_impl_sdlrenderer3; do
    o="$O/obj/$(basename $f).o"
    [ "$o" -nt "$I/$f.cpp" ] || clang++ -O2 -std=c++17 -I"$I" -I"$I/backends" -c "$I/$f.cpp" -o "$o"
done
clang++ -O2 -std=c++17 -I"$I" -I"$I/backends" "$D/zadevtools.cpp" "$O"/obj/*.o -lSDL3 -lpng16 -lz -fuse-ld=lld \
    -Wl,--subsystem,windows -o "$O/zadevtools.exe"
for dll in SDL3.dll libpng16-16.dll zlib1.dll libc++.dll libunwind.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    [ -f /c/msys64/ucrt64/bin/$dll ] && cp -u /c/msys64/ucrt64/bin/$dll "$O/" || true
done
[ -f "$O/devtools.ini" ] || cp "$D/devtools.ini.example" "$O/devtools.ini"
echo "built $O/zadevtools.exe (edit devtools.ini next to it)"

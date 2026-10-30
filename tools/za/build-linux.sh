#!/bin/bash
# ZA Recomp on Linux x86-64, built natively (no container): the start screen, and with GAME=1 the game code from the
# player's own extracted/ (exefs/ + romfs.bin, as the start screen sets them up). Checked on Linux Mint 22.3 (Ubuntu
# 24.04 base). The result needs this machine's glibc or newer; tools/linux/appimage.sh (podman, Ubuntu 22.04) is the
# portable way. No game files or game code are read unless GAME=1, and nothing outside this checkout is written.
#
#   tools/za/build-linux.sh            # -> build/linux/x86_64/za/lasrecomp (run it from the repo root)
#   GAME=1 tools/za/build-linux.sh     # + generated/ from extracted/exefs, then lasgame.so (~20-60 min, ~6 GB RAM)
#
# SDL3 3.4, an LGPL FFmpeg, shaderc and the Vulkan / VMA headers come from pinned sources (tools/linux/build_deps.sh,
# into build/linux/x86_64/deps); the system only provides the compiler and the headers SDL3's backends are built
# against (X11, Wayland, audio, udev). Those are checked first, with the apt command for what is missing (PKGCHECK=0
# skips that: a deps/ prefix already built, e.g. an SDL3 without some backends for a quick test).
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=x86_64
L=$PWD/build/linux/$ARCH
P=$L/deps
B=$L/za
GENOPT="-O1 -DGUEST_VOLATILE"  # (the Windows release's game code flags, tools/za/dist.sh: part of the library id)
JOBS=${JOBS:-$(nproc)}

[ "$(uname -m)" = $ARCH ] || { echo "build-linux: x86-64 only (aarch64: tools/linux/appimage.sh aarch64)"; exit 1; }
grep -qw avx2 /proc/cpuinfo || echo "build-linux: warning: this CPU has no AVX2 - the game code needs it (x86-64-v3)"

# ---- what the system must have: tools, and SDL3's backend headers (pkg-config module -> Debian / Ubuntu package)
missing=()
for t in clang:clang ld.lld:lld cmake:cmake ninja:ninja-build pkg-config:pkg-config nasm:nasm python3:python3 \
         curl:curl git:git; do
    command -v "${t%%:*}" >/dev/null || missing+=("${t#*:}")
done
python3 -c 'import venv, ensurepip' 2>/dev/null || missing+=(python3-venv)
for m in x11:libx11-dev xext:libxext-dev xrandr:libxrandr-dev xcursor:libxcursor-dev xi:libxi-dev \
         xscrnsaver:libxss-dev xfixes:libxfixes-dev xtst:libxtst-dev xkbcommon:libxkbcommon-dev \
         wayland-client:libwayland-dev wayland-protocols:wayland-protocols libdecor-0:libdecor-0-dev egl:libegl-dev \
         libdrm:libdrm-dev gbm:libgbm-dev libpulse:libpulse-dev alsa:libasound2-dev libpipewire-0.3:libpipewire-0.3-dev \
         dbus-1:libdbus-1-dev libudev:libudev-dev ibus-1.0:libibus-1.0-dev; do
    pkg-config --exists "${m%%:*}" || missing+=("${m#*:}")
done
if [ ${#missing[@]} -gt 0 ] && [ "${PKGCHECK:-1}" != 0 ]; then
    echo "build-linux: missing system packages. Install them with:"
    echo "    sudo apt install ${missing[*]}"
    exit 1
fi

# ---- SDL3, FFmpeg (LGPL), shaderc, Vulkan / VMA headers: pinned, hash-checked, built once
tools/linux/build_deps.sh $ARCH
# the Vulkan loader is always the system's (the GPU drivers' own); only its versioned name ships without libvulkan-dev
if [ ! -e "$P/lib/libvulkan.so" ] && ! pkg-config --exists vulkan; then
    sys=$(ldconfig -p | awk '/libvulkan\.so\.1 .*x86-64/ {print $NF; exit}')
    [ -n "$sys" ] || { echo "build-linux: no Vulkan loader (sudo apt install libvulkan1)"; exit 1; }
    ln -sf "$sys" "$P/lib/libvulkan.so"
fi

# ---- the start screen (no game code inside: that is built from the player's files)
make -j"$JOBS" B="$B" DEPS="$P" SHADERC=-lshaderc_shared GENOPT="$GENOPT" "$B/lasrecomp"
if nm "$B/lasrecomp" | grep -q ' f_70'; then echo "build-linux: game code linked into the app"; exit 1; fi
echo "build-linux: $B/lasrecomp"

[ "${GAME:-0}" = 1 ] || exit 0

# ---- the game code, from the player's own extracted/exefs (never shipped: generated/ and lasgame.so stay here)
[ -f extracted/exefs/main ] || { echo "build-linux: GAME=1 needs extracted/exefs/main (your own game files)"; exit 1; }
if [ ! -f tools/venv/.ok ]; then
    python3 -m venv tools/venv
    tools/venv/bin/pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
    touch tools/venv/.ok
fi
[ -f generated/main/main_build_id.txt ] || make gen
# (generated objects take minutes each at -O1 for a few giant functions; JOBS lower on machines with little RAM)
make -j"$JOBS" B="$B" DEPS="$P" SHADERC=-lshaderc_shared GENOPT="$GENOPT" "$B/lasgame.so"
echo "build-linux: $B/lasgame.so"

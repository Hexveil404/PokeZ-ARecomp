# LAS Recomp on Linux (x86-64, aarch64)

Status: builds and runs the game on Vulkan (with your own game files) on x86-64, checked before
each release by `make smoketest`. aarch64 is **experimental**: cross-compiled, its start screen checked under emulation,
the game itself untested on real hardware - community reports decide (see the end). `make appimage` / `make appimages`
package the release AppImages.

## Running the AppImage
    chmod +x LAS.Recomp-<version>-linux-x86_64.AppImage
    ./LAS.Recomp-<version>-linux-x86_64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with AVX2 (2013 or newer).
On Linux 6.7 or newer the renderer also gets page write tracking (userfaultfd + PAGEMAP_SCAN, as Windows' write watch):
its vertex cache, and textures re-checked only where the game wrote. Older kernels run without it (the log says
`[plat] no page write tracking: ...`); `LARECOMP_WRITE_TRACKING=0` turns it off.
The start screen asks for the folder with your own game files and sets them up.

Everything the app writes lives in `~/.local/share/lasrecomp` (`$XDG_DATA_HOME/lasrecomp`): game files (`extracted/`),
saves, seeds, settings (`larecomp.ini`), shader caches, imported fonts. Started without a terminal the log is
`~/.local/share/lasrecomp/lasrecomp.log`, always the latest run; up to five recent runs are kept as
`lasrecomp-backup-*.log` (from a terminal it prints there instead). `LARECOMP_DATA_ROOT=<dir>` uses
another folder.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs; the button prompts follow the controller. For SDL's own drivers (rumble, LEDs and gyro on PlayStation /
Switch pads) the user needs access to `/dev/hidraw*`, which most distributions give with Steam's udev rules
(`steam-devices`, or Steam installed); without them the controller still works through the kernel driver. Started from
Steam with Steam Input on, the game sees Steam's virtual Xbox controller (Xbox prompts).

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package, or run it with
`APPIMAGE_EXTRACT_AND_RUN=1`. On a Steam Deck, put `APPIMAGE_EXTRACT_AND_RUN=1 %command%` in the game's launch options.

## Building from source
Ubuntu / Debian packages (other distributions: the same libraries under their names). SDL3 must be 3.4 or newer
(LAS Recomp loads and saves PNGs through it); where the distribution ships an older one (Debian 13 has 3.2), build SDL3
3.4 from source first - `tools/linux/build_deps.sh` builds the pinned version the AppImage uses (into
`build/linux/<arch>/deps`).

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev glslc vulkan-tools

shaderc is linked the way its pkg-config file says (`libshaderc_shared` on Arch, Fedora, AOSC...); to choose
yourself: `make SHADERC=-lshaderc_shared` (or `-lshaderc`).

Then from the repo root:

    python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
    make -j         # the start screen only (no game code yet): set up the game files there, or put extracted/ here
    make gen        # once: recompile the game from your own game files
    make -j         # -> build/lasrecomp and the game code library build/lasgame.so

Run `./build/lasrecomp` from the repo root: with `extracted/` in the starting folder it uses that folder for
everything (dev mode) instead of `~/.local/share/lasrecomp`.

`ARCH` picks the target, separately from the OS: the generated code differs per arch (aarch64 runs the guest's
FP/SIMD as inline asm, like Apple Silicon; x86-64 lowers it to C with `-march=x86-64-v3`, like Windows). `make gen`
passes the matching `--simd`. A cross build (`ARCH=aarch64` on an x86-64 machine, `SYSROOT=<arm64 root filesystem>`)
keeps its own `generated-aarch64/` and `build/aarch64/`. Distributions that name shaderc's library
`libshaderc_shared.so` need `SHADERC=-lshaderc_shared`.

The randomizer from a source checkout uses LAS-Randomizer's own venv (Python 3.12):

    python3.12 -m venv third_party/LAS-Randomizer/venv
    third_party/LAS-Randomizer/venv/bin/pip install -r third_party/LAS-Randomizer/requirements.txt

## AppImages
Needs podman, and for the other arch's AppImage QEMU user emulation with binfmt (`sudo apt install podman qemu-user
qemu-user-binfmt`).

    make appimage                # this machine's arch -> build/dist/LAS Recomp-<version>-linux-<arch>.AppImage
    make appimage ARCH=aarch64   # on x86-64: cross-compiled
    make appimages               # both

Everything is built in Ubuntu 22.04 containers (`tools/linux/Containerfile`), so the result runs on glibc 2.35+:
SDL3, an LGPL-only FFmpeg (only the movie codecs, as on Windows / macOS), and shaderc from pinned sources
(`tools/linux/build_deps.sh`), then the game against them (`tools/linux/package.sh`). This machine's arch is built
natively in one container of that arch (the release does both archs that way, on an arm64 runner for aarch64).
aarch64 on an x86-64 machine is cross-compiled in the amd64 container against the arm64 image's root filesystem
(`build/linux/aarch64/sysroot`), with only the seed generator helper (`tools/linux/rando_helper.sh`, PyInstaller
builds for the machine it runs on) frozen in the arm64 container under QEMU, where pip also builds oead and
keystone-engine from source the first time (no arm64 wheels on PyPI; cached in `build/linux/aarch64/wheels`). The
generated game code comes from the host (`generated/`, `generated-aarch64/`; `make appimage` runs `make gen` for it
when missing).

Release check of the x86-64 AppImage (`make smoketest`, tools/linux/smoketest.sh, ~7 minutes on this kind of machine):
three launches of the AppImage itself - the start screen; a session from a test save warping to four overworld spots
and every dungeon's entrance; and a seed generated from the in-game Randomizer tab, switched to live, the save loaded
on it, then deleted. Each step is checked in the log and by a screenshot. `build/smoke/report.txt` ends with READY FOR
RELEASE or NOT READY; `build/smoke/sheet.png` shows every screenshot. It needs `extracted/` in the checkout and a test
save outside git (`~/.local/share/lasrecomp-smoketest/save`, or `LARECOMP_SMOKE_SAVE`): copy `save/` from any build
after starting a new game once.

The AppImage holds `lasrecomp`, `rando-gen/`, the start screen font and the bundled libraries (SDL3,
FFmpeg, shaderc). Not bundled: glibc, libstdc++, the Vulkan loader and the GPU drivers (always the system's). No game
files or fonts.

## Differences from macOS / Windows
- Platform layer `runtime/platform/linux.c`: like macOS, the guest window (0x70_0000_0000..0x73_2000_0000) is one
  read/write mapping (`MAP_NORESERVE`), crashes are caught with `sigaction` on an alternate stack.
- The HLE / service / reset tables are ELF sections found through the linker's `__start_` / `__stop_` symbols.
- Renderer: the Windows Vulkan backend unchanged.
- Without the user's Switch fonts the game text uses the bundled BIZ UDPGothic (`third_party/fonts/BIZUDPGothic`),
  sized like the console's font, where macOS / Windows use Hiragino / Yu Gothic; the in-game menu and button letters
  use DejaVu Sans, the system's or the bundled copy (`third_party/fonts/DejaVu`). TrueType is required; the game's font
  engine can't use CFF fonts such as Noto Sans CJK.
- Pages: 4K and 16K kernels (Asahi) work; 64K-page kernels (some ARM servers) are not supported.

## Testing on ARM (aarch64): what to report
The aarch64 AppImage is experimental: built on x86-64, only its start screen has run (emulated): reports from real hardware
(Asahi Linux, Snapdragon laptops, Raspberry Pi 5 and other boards) are what tells whether it works. Please include:

1. **Distribution and kernel**: `cat /etc/os-release | head -3; uname -r; getconf PAGESIZE`
2. **GPU and Vulkan driver**: `vulkaninfo --summary` (package `vulkan-tools`); at least the devices section with
   `deviceName`, `driverName` and `driverInfo`.
3. **What happened**: start screen? extraction? in game? where it stopped, any visual problems (screenshots help).
   With a controller: its model, and whether it works (the log names it: `[input] gamepad: ...`).
4. **The log**: `~/.local/share/lasrecomp/lasrecomp.log` (or the terminal output when started from a terminal).
   A crash writes `*** signal ...` and a backtrace there; attach the whole file (or if the problem was in
   an earlier run, that run's `lasrecomp-backup-*.log`, by date modified).
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./LAS.Recomp-<version>-linux-aarch64.AppImage --appimage-extract    # -> squashfs-root/
       cd ~/.local/share/lasrecomp
       gdb -batch -ex 'handle SIGSEGV nostop noprint pass' -ex run -ex 'thread apply all bt 20' \
           --args ~/squashfs-root/usr/bin/lasrecomp > gdb.txt 2>&1

   (Adjust the `squashfs-root` path to where you extracted it. The game itself uses SIGSEGV for memory watches, hence
   the `handle` line; a real crash still ends in the backtrace. Attach `gdb.txt`.)

## Steam Deck (SteamOS, Pokémon Legends: Z-A)
Measured 2026-10-24 on a Steam Deck (Van Gogh APU, 14 GB RAM, SteamOS kernel 6.18, Mesa 25.2.8 RADV, Vulkan 1.4), Z-A 2.0.0
from the player's own dump, built from this repository on x86-64 Linux. Nothing from the game is in this document.

What to expect: with the Vulkan fixes of pull requests #7-#12 (compute in 32-lane subgroups, index buffers not from the
transform-feedback mirror, depth clamp, the guest-heap import as a window on integrated GPUs, the SSY/PBK scalars and BRX
labels in the shader translator) the opening scene in Lumiose runs at about 29 fps at 1280x800 with the first station drawn
completely (arches, lamps, walls, shadows). Measured over 1500 scripted frames: #7 alone 3 fps, #7-#10 5 fps, #7-#12 29.4 fps.
Without #7 the translated compute shaders can hang the GPU on AMD (`sr failed -4`). The first load of a scene compiles
shaders (a few seconds of skipped draws); later runs reuse the caches.

Build: SteamOS' root file system is read-only and has no compiler, so build in a container (distrobox, Ubuntu 24.04 or
any recent distribution) with the packages above plus SDL3 built from source into a prefix (`DEPS=<prefix>`), then
`make gen` and `make -j8 DEPS=<prefix> B=build/o1 GENOPT="-O1 -DGUEST_VOLATILE"`. clang 18 can sit for an hour on one giant
generated function at -O1: build such objects at -O0. Run `build/o1/lasrecomp extracted --play` inside the container (it
shares the host's display and GPU); add the launcher script to Steam as a non-Steam game to play in Game Mode.
Do not touch a header to force a rebuild: every generated object depends on them (1411 objects, 34 minutes on the Deck).

Switches: `LARECOMP_VK_CS_SUBGROUP=0` leaves compute to the driver's 64-lane waves (wrong culling, possible hang),
`LARECOMP_VK_GMEM_WINDOW=0` imports the whole 3.2 GB heap again (87 % of the render thread then goes into
`vkQueueSubmit2`). `perf record -t <render thread>` sees user space only (perf_event_paranoid=2); the `[vk] t=` log line's
`submit` ms shows the kernel time inside the submit ioctl.

## ZA Recomp on Linux (native, x86-64)
Experimental. ZA Recomp builds and runs natively on Linux x86-64 (no Wine / Proton) and can be packaged as an
AppImage. Most of what makes that possible was already here, from LAS Recomp: the Linux platform layer
(`runtime/platform/linux.c`), the ELF game code library (`lasgame.so`), the Vulkan backend shared with Windows, page
write tracking, the pinned dependency build (`tools/linux/build_deps.sh`), and the Steam Deck fixes (#7-#12) above. What
was added for Pokémon Z-A on Linux:

- FSR 2 compiles outside Windows (`FFX_GCC`, `size_t`, `wcscpy_s`); a dev build linked against `DEPS` gets an rpath.
- Its own data folder, `~/.local/share/zarecomp` (`$XDG_DATA_HOME/zarecomp`), not LAS Recomp's `lasrecomp/` (both
  installed would share saves, game files and `larecomp.ini`).
- A first run copies the release's settings (`za-default.ini`, shipped next to the app) into the data folder as
  `larecomp.ini`, as the Windows zip does with the `larecomp.ini` next to the exe.
- `ooz` (the zaprompts helper's decompressor) builds as `libooz.so` through stand-in Windows headers
  (`tools/za/ooz_posix/`); `third_party/ooz` itself is unchanged.
- `tools/za/build-linux.sh` (native build) and `tools/za/dist-linux.sh` (the Z-A AppImage: zaimport for linux-x64,
  lasgen, zaprompts, zig), the Linux counterparts of `tools/za/dist.sh`.

### Distributions
Checked on Linux Mint 22.3 (Ubuntu 24.04 base). Any x86-64 distribution with glibc 2.35+, SDL3's build dependencies, a
Vulkan 1.3 driver (Mesa RADV / ANV / NVK or NVIDIA's) and a CPU with AVX2 should work; package names below are
Debian / Ubuntu / Mint ones. The AppImage made by `dist-linux.sh` needs the glibc of the machine that built it or newer
(2.39 for Mint 22 / Ubuntu 24.04); the podman route above (`tools/linux/appimage.sh`, Ubuntu 22.04) is LAS Recomp's and
does not package the Z-A helpers yet.

### Dependencies

    sudo apt install clang lld cmake ninja-build nasm pkg-config python3-venv curl git patchelf dotnet-sdk-8.0 \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libdrm-dev libgbm-dev \
        libpulse-dev libasound2-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev libvulkan-dev

SDL3 3.4, an LGPL-only FFmpeg, shaderc and the Vulkan / VMA headers are not taken from the system: `build-linux.sh`
builds them once from pinned, hash-checked sources into `build/linux/x86_64/deps` (`tools/linux/build_deps.sh`).
The system packages above are what SDL3's backends are compiled against; `build-linux.sh` checks them first and
prints the `apt install` line for whatever is missing. `dotnet-sdk-8.0` is only needed for the AppImage (zaimport is
published self-contained, so players need no .NET); `patchelf` likewise.

### Building

    tools/za/build-linux.sh          # the start screen: build/linux/x86_64/za/lasrecomp (run it from the repo root)
    GAME=1 JOBS=3 tools/za/build-linux.sh   # + the game code (generated/ and lasgame.so) from your own extracted/
    tools/za/dist-linux.sh           # build/dist/ZA Recomp-<version>-linux-x86_64.AppImage (ZAIMPORT=0: no importer)

The app is built with the Windows release's game code flags (`GENOPT="-O1 -DGUEST_VOLATILE"`, x86-64-v3).
`GAME=1` compiles giant functions at -O0 first, with the start screen's own rule (`runtime/gamebuild.c` cold_giant),
so no single file holds the build for an hour. Each compiler process takes up to ~1.1 GB: lower `JOBS` on machines
with little RAM.

### Game files (your own)
The start screen (AppImage) reads your .xci / .nsp packages (base game, update, DLC) with your `prod.keys` through
zaimport, or an already extracted `exefs/` + `romfs/`, and builds the game code itself. For a source checkout, the same
importer can write `extracted/` (dev mode: `extracted/` in the starting folder is used as the data folder):

    build/linux/x86_64/zaimport/zaimport <prod.keys> extracted <base.nsp> [<update.nsp>] [<dlc.nsp>...]
    GAME=1 JOBS=3 tools/za/build-linux.sh
    build/linux/x86_64/za/lasrecomp extracted --play

(`dist-linux.sh` builds zaimport into `build/linux/x86_64/zaimport/`.) Never commit `extracted/`, `generated/` or keys.

### Tested (2026-10-30)
Linux Mint 22.3, kernel 7.0, X11 / Cinnamon; Intel Core i5-13420H (8 cores / 12 threads); Intel UHD Graphics
(Raptor Lake-P), Mesa 25.2.8 ANV, Vulkan 1.4; 8 GB of RAM (7.4 GB usable). Pokémon Legends: Z-A 2.0.2 from the
player's own packages (base game + update).

- Start screen, native and as an AppImage: opens, renders, finds its fonts and resources.
- zaimport (linux-x64): base game + 2.0.2 update applied (build ID b1f12fd919eae86a).
- `make gen`: 2 minutes 13 s, 4.4 GB peak. Game code: 1448 objects (160 giant functions at -O0), 20 minutes with
  3 jobs; the library loads with ~270,000 functions (230,332 main + 40,069 sdk).
- Two scripted ~4-minute sessions without input: the language selection and the opening cutscenes rendered correctly,
  no errors in the log. Audio output opened (48 kHz stereo).
- Played by hand by the tester: audio without noticeable stutter, movement works, occasional frame-rate dips.
- Frame rate (window 1280x720, FSR 1 + SMAA): menus at 60 fps; 3D scenes about 15-26 fps at `resolution = match`,
  down to about 10 fps in the heaviest scene; 10-16 fps at the release's `resolution = 1.5x`. Not a steady 30 fps on
  this integrated GPU.

### Known limitations and untested
- Not yet tested: DualShock 4 and other controllers (rumble / LEDs / gyro need access to `/dev/hidraw*`, usually
  Steam's udev rules), saving and loading, mods, other GPUs (AMD / NVIDIA desktop), long sessions, and the AppImage's
  own full setup flow ("Prepare the game": zaimport + lasgen + zig inside the app; it picks
  `min(cores, RAM GB - 2)` jobs, 5 on an 8 GB machine).
- `za-default.ini` sets `resolution = 1.5x`, chosen for dedicated GPUs; on an integrated one `match` ran about 50 %
  faster here.
- 8 GB of RAM is the minimum: the generator peaks at 4.4 GB and the game ran in short sessions only.

### Problems found and their fixes
- `'__declspec' attributes are not enabled` building FSR 2: `-DFFX_GCC` (Makefile, Linux only).
- `Couldn't find dependency package for XSCRNSAVER` building SDL3: the system headers above (`libxss-dev`, and the
  audio / udev ones, without which SDL3 builds with no sound).
- `-lvulkan` not found: `libvulkan-dev` (the loader's unversioned name; the loader itself is always the system's).
- The start screen in a tiny bitmap font: run a dev build from the repo root (its resources are looked up from the
  starting folder; the AppImage keeps them next to the executable).
- A black screenshot in a test run: a fade between scenes (the log shows about one draw per frame there).

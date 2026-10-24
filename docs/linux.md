# LAS Recomp on Linux (x86-64, aarch64)

Status: branch `linux-beta2`. Builds and runs the game on Vulkan (with your own game files) on x86-64, checked before
each release by `make smoketest`. aarch64 is **experimental**: cross-compiled, its start screen checked under emulation,
the game itself untested on real hardware - community reports decide (see the end). `make appimage` / `make appimages`
package the release AppImages.

## Running the AppImage
    chmod +x LAS.Recomp-beta_4-linux-x86_64.AppImage
    ./LAS.Recomp-beta_4-linux-x86_64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with AVX2 (2013 or newer).
The start screen asks for the folder with your own game files and sets them up.

Everything the app writes lives in `~/.local/share/lasrecomp` (`$XDG_DATA_HOME/lasrecomp`): game files (`extracted/`),
saves, seeds, settings (`larecomp.ini`), shader caches, imported fonts. Started without a terminal the log is
`~/.local/share/lasrecomp/lasrecomp.log` (from a terminal it prints there instead). `LARECOMP_DATA_ROOT=<dir>` uses
another folder.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs; the button prompts follow the controller. For SDL's own drivers (rumble, LEDs and gyro on PlayStation /
Switch pads) the user needs access to `/dev/hidraw*`, which most distributions give with Steam's udev rules
(`steam-devices`, or Steam installed); without them the controller still works through the kernel driver. Started from
Steam with Steam Input on, the game sees Steam's virtual Xbox controller (Xbox prompts).

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package (the AppImage runtime
needs `fusermount3`), or run it as `./LAS.Recomp-...-linux-x86_64.AppImage --appimage-extract-and-run`.

## Building from source
Ubuntu / Debian packages (other distributions: the same libraries under their names; SDL3 3.2 or newer):

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev glslc vulkan-tools

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
Needs podman, and for aarch64 QEMU user emulation with binfmt (`sudo apt install podman qemu-user qemu-user-binfmt`).

    make appimage                # this machine's arch -> build/dist/LAS Recomp-<version>-linux-<arch>.AppImage
    make appimage ARCH=aarch64   # cross-compiled from x86-64
    make appimages               # both

Everything is built in Ubuntu 22.04 containers (`tools/linux/Containerfile`), so the result runs on glibc 2.35+:
SDL3, an LGPL-only FFmpeg (only the movie codecs, as on Windows / macOS), and shaderc from pinned sources
(`tools/linux/build_deps.sh`), then the game against them (`tools/linux/package.sh`). aarch64 is cross-compiled in
the amd64 container against the arm64 image's root filesystem (`build/linux/aarch64/sysroot`). The seed generator
helper (`tools/linux/rando_helper.sh`, PyInstaller) is frozen in a container of its own arch; on arm64 that runs
under QEMU and builds oead and keystone-engine from source the first time (no arm64 wheels on PyPI; cached in
`build/linux/aarch64/wheels`). The generated game code comes from the host (`generated/`, `generated-aarch64/`;
`make appimage` runs `make gen` for it when missing).

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
- Without the user's Switch fonts the game text uses DejaVu Sans (TrueType; the game's font engine can't use CFF
  fonts such as Noto Sans CJK).
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
   A crash writes `*** signal ...` and a backtrace there; attach the whole file.
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./LAS.Recomp-beta_4-linux-aarch64.AppImage --appimage-extract    # -> squashfs-root/
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

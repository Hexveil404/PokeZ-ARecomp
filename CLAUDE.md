# ZA Recomp: notes for Claude

A static-recompilation PC port of Pokémon Legends: Z-A, forked from LAS Recomp (Zelda: Link's Awakening, by
Peiikai). The game's ARM64 code is turned into C ahead of time; the runtime re-implements the Switch's OS and its GPU
(NVN -> Vulkan). Supported game versions: 1.0.0, 2.0.0, 2.0.2. Development branch: `pokemon-za`.

Machine-specific paths (game files, install folder) are in `CLAUDE.local.md` when present.

## Where things are

- `tools/gen.py`, `lifter.py`, `analyze.py`, `patches.py`, `simd_c.py`, `flagfuse.py`, `za_patches.py`: the generator,
  which turns ARM64 into C. Players' machines run it from the start screen (`runtime/gamebuild.c`). `flagfuse.py`
  fuses a compare with its branch when nothing else reads the flags (a flag-liveness pass; Z-A's battle-loading freeze
  1.36 s -> 0.6 s); `regpromote.py` keeps x19..x28 in C locals (callee-saved: written back only at exits). (`tools/passes.py`
  is a log summarizer, not part of the generator.)
- Dev rebuild of the game code: `cd tools && PYTHONUTF8=1 ../build/gen-venv/Scripts/python.exe gen.py main
  ../testroot/extracted/exefs/main 0x7000000000 ../generated/main --simd c` (and `sdk ... 0x7008000000 ../generated/sdk
  --hle ../runtime/hle/hle_symbols.txt`), then `make ... build/za-dist/lasgame.dll` (~20 min).
- `runtime/kernel.c`: threads, mutexes and condvars, the address arbiter, the memory map, SVCs.
- `runtime/services/fs.c`: the RomFS, the mod layers (`mods/<name>/romfs`, with `arc/data.trpfd` unlisting) and the
  save files (`save/save_0/`).
- `runtime/nvn/rqueue.c`: the render thread. `nvnQueueSubmitCommands` queues the call's lists (one item:
  `rq_submit_lists`) and returns; the render thread translates them. Fences are item numbers.
- `runtime/nvn/vulkan/exec.c`: command-list translation, per draw: pipeline key (with a memo), descriptor sets (reused
  while their contents repeat), uniforms. Also GMEM_FAST (compute-buffer copies with a merge copy-back,
  `gfast_merge.comp`), the vertex cache, page-write tracking, and Z-A's guest window (`gwin`: Z-A has no heap).
- `runtime/nvn/vulkan/record.c`: the recorder thread. Every `vkCmd*` / descriptor / queue call goes through a `rec_*`
  wrapper - never call `vkCmd*` directly from translation code (a race with the recorder).
- `runtime/nvn/vulkan/textures.c`: texture upload and change checks. Unpacking (deswizzle) is queued and done on a
  few threads just before the submission (`vk_tex_unpack_finish`, `LARECOMP_TEX_UNPACK_THREADS`). Textures are
  uploaded when the game registers them (`backend_prefetch_texture`, a render-queue item from
  `nvnTexturePoolRegisterTexture`; `LARECOMP_TEX_PREFETCH=0` off): a battle's textures load during its loading
  freeze, not on its first frame. Costs ~350 MB more GPU memory.
- `runtime/nvn/shader_prep.c` / `shader_seen.c`: shader preparation. Z-A records every variant it builds
  (`cache/shader_seen.txt`) and its shader code (`cache/shader_code.bin`, the player's own data, never shipped), and
  prepares them at its GPU device setup (`shader_prep_boot`, ~0.2 s warm; `LARECOMP_SHADER_PREP=0` off). Not from the
  start screen: before boot the guest memory window is Zelda's and the translations would bake the wrong one in.
- `runtime/crash_explain.c`: the crash dialog's plain-language explanation.
- `runtime/nvn/vulkan/device.c`: present, `[mem]` and `[scale]` log lines, screenshots.
- `runtime/nvn/maxwell/translate.c`: Maxwell shader -> GLSL/MSL.
- `runtime/ui/launcher.mm`: the start screen (game-file setup, save import, update check).
- `runtime/ui/menu.mm`: the F1 settings menu.
- `runtime/hle/*`: host-side function hooks. The Zelda-only ones (warp, souls, digs, rando, cheats...) read Zelda
  addresses through `GA()` (`runtime/game_addresses.h`); for Z-A that table is Zelda 1.0.0's and the hooks are gated
  off by `g_game_is_la`. Any new per-frame or per-draw code that reads or writes `GA()` must check it.
- Merged from upstream LAS Recomp (`peiikai` remote) on 2026-10-26: Z-A-specific decisions are marked "Pokémon Z-A"
  in comments (saves written in place, no heap chunks, pacer off, `tools/za_patches.py`).
- Issue numbers in comments that came from upstream (`issue #205`, `#19`, ...) are LAS Recomp's
  (github.com/peiikai/LASRecomp/issues), not this repository's: kept as they are so upstream merges stay clean.
  Link's Awakening-only code (randomizer, cosmetics, hint sheet, LiveSplit, `runtime/nvn/shader_variants.txt`...) is
  kept for the same reason; it is gated off for Pokémon Z-A (`g_game_is_la`, and the menus' `menu_la_rows()`).
- `runtime/ui/launcher.mm` is ZA Recomp's own start screen (not upstream's): upstream launcher changes are ported by
  hand. Never delete `romfs.bin` there - for Pokémon Z-A it is the game's only data (zaimport writes no romfs/).
- `tools/za/`: Z-A scripts (perf runs, log watcher, stack symbolizer, texture and font tools).
- `tools/devtools/`, `tools/za/replay-*.sh`, `runtime/ui/devtools.mm`: ZA DevTools, record and replay, frame capture
  analysis (`docs/dev-tools.md`). Off unless `LARECOMP_DEVTOOLS=1` or their own variables are set.

## Building (Windows, Git Bash)

One-time setup: `tools/setup_windows.ps1` (MSYS2 UCRT64 toolchain). Fast exe rebuild:

    (export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make -j"$(nproc)" B=build/za-dist \
      DEPS="$PWD/build/deps-win" RELEASE=1 GENARCH=-march=x86-64-v3 GENOPT="-O1 -DGUEST_VOLATILE" \
      build/za-dist/lasrecomp.exe)

- Keep the PATH change in a subshell: an exported MSYS PATH breaks `cmd` in later commands.
- `build/za-dist/lasrecomp.exe` has no DLLs next to it, so it can't start from there. Copy `lasrecomp.exe` and
  `lasrecomp.dll.a` into `build/dist-za/ZA Recomp/` (the test copy) and run that.
- **`GAMELIB_ID`** (Makefile) hashes the generator sources, `cpu.h`, `simd*.h`, `gamelib.h`, `hle_symbols.txt` and the
  flags. Changing any of them makes every player recompile the game (10-30 min). Runtime, Vulkan and UI changes
  don't, so prefer them.
- LF line endings everywhere (`core.autocrlf=false`, set by the setup script).

## Testing and debugging

- Test windows ignore keyboard and controller. Warn the user before starting one, and never kill or replace their
  running game.
- `tools/za/perf-win.sh`: a scripted run on a scratch copy (never touches the player's save, settings or caches).
  Takes `GAME=`, `EXE=`, `IN=@<recorded input>`, `SAVE=`, `INI=`, `T=`. Settings and data paths must be absolute
  Windows paths, or the run silently uses defaults.
- Record input: `LARECOMP_RECORD_INPUT=<file>`. Replay: `LARECOMP_INPUT=@<file>`.
- `GAME=<install> tools/za/watch-log.sh`: follows `logs/play.log` live (hitches, errors, crashes, `[shot]`).
- Profiling:
  - `LARECOMP_VK_PROF=1`: ms per second by category in the fps line (recorder, set reuse, key memo stats too).
  - `[slow]` lines: every frame over `LARECOMP_SLOW_FRAME_MS` (default 50) with what it did.
  - `LARECOMP_SAMPLE=<delay>,<dur>` with `LARECOMP_SAMPLE_EXEC=1`: stack sampling of the command-list thread into
    `build/stacks.txt`; symbolize it with `tools/za/stacks.py`. Without `_EXEC`: every thread's instruction pointer
    (~30 Hz each) into `<data root>/build/samples.txt`, for `tools/za/samples.py`. Run with `LARECOMP_RENDER_THREAD=0`
    to stack-sample the game thread.
  - `LARECOMP_PROF_SVC=1`: kernel calls over 10 ms, and per-thread call counts (`[svcprof]`).
  - `LARECOMP_LOG_READS=<file>`: every RomFS read with its time and duration.
- Windows: never `VirtualQuery` inside the guest window on a hot path. It measures the run of like pages, GBs here,
  ~15 ms a call (it made every file read 3 ms and every first touch of guest memory slow until 2026-10-27).
- Test runs read an ini with `fps60 = 0`: pass `LARECOMP_FPS=60`, or the fps line shows 30.
- In game: F1 opens the menu, F9 dumps the next frame's passes, F10 saves a screenshot to `screenshots/`.
- Kill switches for recent changes, to bisect visual bugs:
  - `LARECOMP_VK_GMEM_FAST=0`, `LARECOMP_VK_GFAST_MERGE=0`, `LARECOMP_IB_GUARD=0`, `LARECOMP_VK_CLAMP_COLOR=1`,
    `LARECOMP_WARP_EMU=0`
  - `LARECOMP_VK_SUBMIT_EACH=1`, `LARECOMP_VK_BATCH_DRAWS=<n>`
  - `LARECOMP_RENDER_THREAD=0` (translate on the game thread), `LARECOMP_VK_RECORDER=0`, `LARECOMP_VK_NO_SET_REUSE=1`,
    `LARECOMP_VK_NO_KEY_MEMO=1`, `LARECOMP_VK_NO_CBUF_LEAN=1`
  - `LARECOMP_VK_PAGES_OUTSIDE=0`, `LARECOMP_VK_NO_SLOT_MEMO=1`
- Unit and fuzz tests exist (`tools/test_translate.sh`, `test_vk_pipelines.sh`, `test_lift.py`, `test_simd.py`,
  `test_flagfuse.py`, `test_regpromote.py`) but there is no CI. Most checks are real game runs.

## Rules

- Never commit or attach Nintendo files: game files, `extracted/`, `generated/`, fonts, textures, saves.
- Mods from GameBanana etc. are not ours to bundle. They go in the player's `mods/` folder only.
- Comments follow the existing style: dense, explaining *why*, with parenthetical asides and the env var that
  changes the behaviour. Match it.
- Guest threads are real host threads, around 150 in Z-A. Any shared kernel state needs a lock (`g_klock`, or
  `g_mlock` for the memory map).

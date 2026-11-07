<div align="center">

# Pokémon Legends: Arceus on ZA Recomp

**Experimental.** The same static recompiler and runtime, now running a second Game Freak game.

**English** · [Português (Brasil)](ARCEUS.pt-BR.md)

</div>

---

ZA Recomp was built for *Pokémon Legends: Z-A*. *Pokémon Legends: Arceus* runs on the same engine family, and with
the changes on this branch it plays natively on PC: its ARM64 code is recompiled ahead of time to C, the Switch's
OS and GPU are reimplemented on Vulkan, and no emulator runs while you play.

> [!IMPORTANT]
> **You need your own copy of the game** (base game and the 1.1.1 update, dumped from your Switch, with your
> `prod.keys` / `title.keys`). Nothing from the game is included here: no code, assets, fonts or saves.

> [!WARNING]
> This is early work. It has been played on **Linux** (Intel iGPU) from a new game through Jubilife Village. Windows
> and macOS builds have not been tried with Arceus yet.

## What works

| | |
|---|---|
| Boot, language menu, intro, name entry | ✅ |
| Obsidian Fieldlands beach, Laventon and the three starters | ✅ |
| Jubilife Village, Galaxy Team HQ, story dialogue | ✅ |
| Characters, dialogue text, exposure / lighting | ✅ |
| Frame rate (Intel Raptor Lake iGPU, 75 % resolution) | ~30 fps |
| Fan translations and other `romfs` mods | ✅ (`mods/<name>/romfs`) |
| Keyboard, controller (SDL3) | ✅ |

## Supported version

| Game | Title ID | Version | `main` build ID |
|---|---|---|---|
| Pokémon Legends: Arceus | `01001F5010DFA000` | **1.1.1** (update v262144) | `AEE8F150DDA1B5A8` |

Other versions are not recognized: the generator and the start screen check the build ID.

## Setting it up

1. Put your packages in one folder: the base game `.nsp` / `.xci`, the 1.1.1 update `.nsp`, and your keys.
2. Open ZA Recomp and pick that folder on the start screen. `zaimport` reads Arceus's packages as it does Z-A's and
   sets up `exefs/` and `romfs.bin`.
3. Let the start screen build the game code. It generates `main`, `sdk` and Arceus's two extra modules (`subsdk0`,
   `subsdk1`), then compiles them (10 to 60 minutes, once).
4. Play.

### Translations and mods

Mods that replace game files go in `mods/<name>/romfs/` (or through the **Mods** page). For example, a fan translation
whose archive contains `01001F5010DFA000/romfs` goes in `mods/<any name>/romfs`. Mods are never bundled: get them from
their authors.

## What changed to make it run

Arceus differs from Z-A in a few places the runtime had assumed fixed:

- **Two extra code modules.** Arceus ships `subsdk0` and `subsdk1`. The loader loads them at `0x700a000000` /
  `0x700b000000`, the generator recompiles them, the game library lists them (`GAMELIB_ABI 2`), and the start screen
  builds them.
- **A 32 GB address-space reservation.** Its main allocator reserves 32 GB in one piece, so the alias region grew to
  48 GB, up to the top of the 39-bit address space.
- **A heap and a physical pool at once.** Z-A has no heap; Link's Awakening has only a heap. Arceus sets a small heap
  and keeps its graphics memory in a 3.1 GB physical pool. The GPU's guest-memory window now takes the larger one.
- **Storage buffers spread lower in that pool.** On integrated GPUs only the top of the pool is imported for shader
  global memory; for a game with a heap that is now the top 1 GB (Arceus's glyph instances and exposure buffers sit
  there). Importing the whole pool fixed the text too, but pinned 3.2 GB and stalled a 7.4 GB machine in swap.
- **A jump table the generator could not resolve.** Its index is loaded through memory before the bounds check; the
  generator's emulation now skips a faulting load there instead of giving up.
- **Services**: `friend:u/a/v` (an offline Switch with no friends).
- **Debugging**: F9's frame dump now captures the whole frame with the render thread on.

## Known issues

- A rare crash in the game's worker threads (an object looked up with an invalid id). Seen once in Galaxy Team HQ,
  not reproduced since.
- One fragment shader with a 64-bit atomic is not translated (`atom size 2`); nothing visible depends on it so far.
- The start screen's full build of Arceus (from scratch) has not been run end to end yet; the library used for testing
  was built with `tools/za/build-linux.sh`.

## Reporting a problem

Open an issue with `logs/play.log` (or the log printed in the terminal), what you were doing, and a screenshot (F10).
For a hang, the log has a `[hang]` dump; for a crash, a `[crash]` report.

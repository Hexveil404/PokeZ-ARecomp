# Developer tools

Tools for working on ZA Recomp itself: **ZA DevTools**, record and replay, full frame captures and the scripts that
read them. Players don't need any of this. Everything here is off unless you turn it on, so a normal run behaves the
same with or without it. Windows only for now, except the replay scripts' runtime side.

> **Never commit or attach Nintendo files:** keys, game files, `extracted/`, saves, or captures and screenshots of the
> game. A recording holds a copy of a save, and a full capture holds game data. Keep both out of git and out of issues.

Portuguese: [dev-tools.pt-BR.md](dev-tools.pt-BR.md).

## Setup

You need your own game files, already set up (`exefs/` + `romfs.bin`) and a compiled `lasgame.dll`.

1. Build the game the usual way (see `CLAUDE.md`, "Building").
2. Put `lasrecomp.exe` in a folder of its own with its DLLs. The file must keep that name: the game code imports the
   runtime as `lasrecomp.exe`, so a renamed exe loads a second runtime and crashes at boot.
3. Build the tool:

   ```bash
   bash tools/devtools/build.sh
   ```

   This creates `build/devtools/zadevtools.exe` and a `devtools.ini` next to it.
4. Fill in `devtools.ini`. It stays out of git, because it holds your paths:

   | Key | What it is |
   |---|---|
   | `work` | Folder for sessions, repros and captures. The game runs with it as its working folder. |
   | `exe` | The `lasrecomp.exe` from step 2. |
   | `game` | Your game files. |
   | `lib` | The `lasgame.dll` to use (the "DLL: dev" choice). |
   | `release_lib` | Optional: one built by the start screen (the "DLL: release" choice). |
   | `profile` | `name \| save folder \| settings file`, one line per save. |

## ZA DevTools

Start `zadevtools.exe`, pick a profile and press **Start**. It starts the game for you, with its own save copy and
`LARECOMP_DEVTOOLS=1`, and keeps what it received after the game closes or crashes. It reconnects by itself.

To attach it to a game you started yourself, set `LARECOMP_DEVTOOLS=1` and press **F10** in the game. The game looks for
`zadevtools.exe` next to its own exe, or at the path in `ZADEVTOOLS`. Without that variable F10 stays the screenshot key.

| Tab | Use it for |
|---|---|
| Log | The live log, with tag filters, search and save. |
| Performance | Frame times (average, 1%, worst), fps, render wait and draw counts. A drop trigger saves `captures/<time>/`. |
| Frame | Browsing a capture: passes, draws, shaders, and which draws touched a spot you click. |

Two switches change how the game runs:

- **Race test** sets `LARECOMP_SYNC_SUBMIT=1`, so each command list waits for the GPU. A bug that goes away with it is
  a CPU / GPU race.
- **DLL: dev / release** picks the game library, to compare a dev build with what players get.

### Reproducing a bug

Every run started from the tool is recorded in `testes/sessions/<time>/` under `work`.

1. Play until the bug is on screen, then press **Mark bug here**. The session becomes `repros/<time>/`.
2. Press **Replay to the bug**. A fresh copy of that save runs the recording up to the mark, and the controls are yours
   from there.
3. To open a repro straight away: `zadevtools.exe --replay <repro name>`. `--start <profile number>` starts a profile.

## Record and replay from a script

```bash
export EXE=/c/za-dev/bin/lasrecomp.exe GAME=/c/za-dev/game LIB=/c/za-dev/game/lasgame.dll
SAVE=/c/za-dev/saves/test/save bash tools/za/replay-record.sh city        # play, then close the game
bash tools/za/replay-play.sh city 120                                     # replay for up to 120 s
```

The recording goes in `replays/<name>/` under `WORK` (default: the current folder), and the replay log in
`replays/<name>/run/replay.log`. `SHOT=<frames>` saves screenshots, and `SAMPLE=<delay,dur>` profiles the run.

Keep these limits in mind:

- Replays follow the game's own controller reads, so they are not pixel-exact. Two replays of one build can differ a lot
  in a picture comparison.
- A recording can drift after a game code or timing change, and the player ends up walking into a wall. Record again.
- Replays use the recorded settings. Without `fps60 = 1` in them, the game runs at 30 fps.
- Don't keep a shell inside `replays/<name>/run`: it blocks the cleanup at the next replay.

## Frame captures

| How | Output |
|---|---|
| **F9** with `LARECOMP_DEBUG_FRAMES=1` | One image per pass: `build/pass_<frame>_NNN.png`. |
| **F9** with `LARECOMP_FULLCAP=1` too | `build/fullcap_<frame>.bin`, about 170 MB: colour and depth grids around each draw, constant buffers, storage, vertex streams, textures, compute dispatches. |
| `LARECOMP_DEBUG_FRAME=<frame>` | The same at a chosen frame, with no key press. |
| `LARECOMP_DUMP_GLSL=1` | Translated shaders in `build/glsl/`. |

The grids come in pairs, before and after each draw, so a change can be pinned on the exact draw.

### Analysis scripts

`tools/devtools/analysis/` reads full captures. `fcread.py` is the loader and the others build on it:

```bash
python -I tools/devtools/analysis/cells.py tools/devtools/analysis build/fullcap_3510.bin <draw>
```

| Script | Answers |
|---|---|
| `cells.py`, `cells2.py` | Which draw painted each grid cell in a pass? |
| `who.py` | Which draws touched an address range? |
| `mesh.py`, `strides.py`, `brute.py` | What do a draw's vertices look like, and what layout fits them? |
| `inst.py`, `argscheck.py` | Are GPU indirect arguments and instances sane? |
| `clip.py`, `raster.py`, `raster_inst.py` | Where do a draw's triangles land on screen? |
| `cb9.py`, `sb0.py`, `d90.py` | Dumps of specific buffers, from past bug hunts: starting points to adapt. |

## Debug switches

| Variable | Effect |
|---|---|
| `LARECOMP_HIDDEN=1` | Runs with no window on screen, for scripted tests. |
| `LARECOMP_INPUT=<frame:BUTTON:frames,...>` | Scripted pad input. |
| `LARECOMP_SHOT_AT=<frames>` | Saves the game image at those frames in `build/frames/`. |
| `LARECOMP_SHADER_FLAT=0/1` | Straight-line shader form off / on. |
| `LARECOMP_SHADER_QUAD=0` | Derivatives through subgroup ids instead of quad operations. |
| `LARECOMP_VK_INTEL_RENDER_PASSES=0/1` | Render passes instead of dynamic rendering. |
| `LARECOMP_POOL_WATCH=<addr>:<len>` | Logs what changes a guest memory range. |
| `LARECOMP_ARGS_TRACE=<frame>:<lo>:<len>` | Logs changes to indirect draw arguments. Slow. |
| `LARECOMP_FS_MBPS=<n>` | Limits RomFS reads to the speed of slow storage. |

`CLAUDE.md` has the list of kill switches and profiling options.

## Logs

| File | What it holds |
|---|---|
| `logs/launcher.log` | Start screen, setup and build history, kept across starts. Ask for this one for setup and build failures. |
| `logs/play.log` | The latest run. The run before it is `logs/play-anterior.log`. |
| `lasrecomp-backup-*.log` | Runs from before the game is set up, next to the exe. |

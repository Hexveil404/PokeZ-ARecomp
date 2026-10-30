# Xbox attack layout

## For players

Install the complete release ZIP, keeping its `zaprompts/` helper next to the
executable. Import your own game through the normal start screen. Setup generates
the private Xbox UI resource automatically, even while the option is off.
There is no extra Python installation, script command, resource download or
source compilation for this feature.

Open **Settings > Controls**, leave **Battle moves by position** off, enable
**Xbox attack layout** / **Layout de golpes Xbox**, then restart. The setting is
off by default. With letter-based Xbox bindings, A appears below, B right,
X left and Y above. The complete move containers are reassigned; the inputs,
move selection and bindings are unchanged.

The layout follows the configured four distinct physical face-button bindings
when the game starts. Restart after changing bindings or either layout option.
To restore the original HUD, turn the Xbox option off and restart.

The ordinary four-move HUD is covered. Separate finishing/last-move interfaces
are unchanged. New game layouts, all languages, effects and controllers are not
claimed to be exhaustively covered.

## Setup, existing installs and updates

The shipped `zaprompts` program already reads the player's RomFS and contains its
own Python runtime and ooz. The launcher now passes `--xbox-attack-layout` along
with its existing controller-icon preparation request. It produces:

```text
mods/_XboxAttackLayout/romfs/ui/data/btl_app/btl_app_waza_00.arc
mods/_XboxAttackLayout/resource.bin
mods/_XboxAttackLayout/resource.json
mods/.XboxAttackLayout.prepared
```

These are local player-derived files; never commit, attach or distribute them.
The underscore intentionally hides the overlay from normal Mods scanning.
Only the Xbox setting mounts it. The release ZIP contains the preparer, not
any of these generated files.

Existing installations are prepared when the updated launcher sees an old
helper/source stamp or missing/incomplete assets, even if controller icons
already exist. The stamp includes the helper format version, controller/key
bindings, the main NSO build ID, and the size/mtime of the source RomFS and main executable. Replacing
or updating the game therefore schedules regeneration. Runtime content guards
remain authoritative if source timestamps are preserved.

Preparation runs in the existing setup flow and waits there. Normal startup
can prepare in the background; when Xbox layout is enabled and still missing,
both Play and direct boot wait for that helper before mounting RomFS.
Controls and graphics execution are not intercepted.

## Compatibility and failure behavior

The helper validates the catalog association, SARC structure, four unique
parent names, geometry, direction-specific templates and animation references.
It retains all archive bytes unchanged. Runtime metadata describes the source
packed range, archive size/hash and four name offsets; it is loaded at startup,
not compiled into the executable. A supported update can be prepared using the
same binary.

The runtime independently bounds and verifies the packed source bytes, metadata
and local archive before changing four final name digits in memory. Unknown or
incompatible data, missing files, invalid/duplicate face bindings, or the old
position-remapping option leave the original HUD in use. Reasons are logged as
`[xbox-layout]`; setup details are in `textures/prompts.log`.

An incompatible layout is remembered only for the current source/helper stamp
so the launcher does not retry forever. I/O failures remain retryable within
the existing three-attempt limit. Source/helper updates reset that limit.
Preparation validates before writing, stages a complete archive/metadata pair,
then switches the private folder. Repeated preparation does not replace an
identical archive. A failed move rolls back the previous copy; if rollback
itself fails, the private backup is retained and the runtime fails closed.
No original game file, save, setting or shader cache is overwritten.

Updating only `lasrecomp.exe` is insufficient if `zaprompts` is still an older
helper. Use the complete release ZIP. Maintainers must rebuild and ship both.

## Implementation

The catalog maps `ui/data/btl_app/btl_app_waza_00.arc` to
`arc/uidatabtl_appbtl_app_waza_00.arc.trpak`. Its SARC member
`blyt/btl_app_waza_00.bflyt` contains:

| Original parent | Position | Child template |
|---|---|---|
| L_waza_00 | top | btl_app_waza_parts_01 |
| L_waza_01 | right | btl_app_waza_parts_00 |
| L_waza_02 | left | btl_app_waza_parts_02 |
| L_waza_03 | bottom | btl_app_waza_parts_01, text below |

The original game updates these parents by name. For Xbox letter bindings,
top/right/left/bottom identities become 02/03/00/01. The direction-specific
templates keep text and effects oriented outward; translating coordinates
alone would not preserve that. Textures, child layouts, animation data, colors
and other resource bytes are unchanged.

`runtime/xbox_attack_layout.h` keeps the original CPU mapping/relabeling.
`runtime/xbox_attack_resource.h` defines the versioned 80-byte little-endian
metadata format, size limits and checked decoder. It contains no per-game
addresses and is never generated on the player's machine.

`tools/za/xbox_attack_layout.py` is an importable preparation module bundled by
`tools/zaprompts.spec`. It uses the helper's existing ONEPACK readers and ooz;
it does not read source files or generate C headers. The release script checks
the frozen helper's capability and includes this document in the ZIP.

## Maintainer validation

Normal build prerequisites remain unchanged. The release helper is built by
`tools/za/dist.sh` using the existing pinned Python dependencies. Its internal
`--xbox-attack-layout-version` mode checks packaging without game data;
`--xbox-attack-layout-only` allows an isolated resource smoke test. Neither is
required of players.

```sh
python -B tools/test_xbox_attack_layout.py
python -B tools/test_xbox_attack_prepare.py
python -B tools/test_xbox_attack_launcher.py
python -B tools/test_settings.py
python -B tools/test_windows_processes.py
make -j4 GENOPT='-O1 -DGUEST_VOLATILE' GENARCH=-march=x86-64-v3 build/lasrecomp.exe
```

Runtime tests cover all 24 valid face permutations, 1272 invalid combinations,
metadata bounds/overflow/schema, byte preservation, corrupt inputs, settings
roundtrips and production filesystem preparation. Preparation tests use
synthetic layouts to cover idempotence, compatibility, updates, repair,
rollback, invalid stamps and retryable I/O. Launcher tests execute the actual
scheduling functions with process stubs on temporary files, including existing
installs, missing assets, updates, retries and blocking when enabled.

`tools/test_xbox_attack_release.py` additionally takes a frozen helper,
compiled executable and a read-only local installation to build/extract a
release-component test ZIP. It exercises the normal helper invocation with no
Python/compiler on PATH, then checks the generated resource with the production
CPU/FS tests. No game files are put in the ZIP and the game is not launched.

Preparation has been tested on Windows. The contributor previously tested the
visual feature in game; the new automatic setup path and PT/EN option still
need an in-game/release installation smoke test. This feature is independent of
the Vulkan investigation and does not claim to fix a GPU page fault.

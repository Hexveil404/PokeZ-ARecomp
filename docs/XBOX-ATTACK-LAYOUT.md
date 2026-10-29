# Xbox attack layout: technical notes

An optional presentation layout for the ordinary four-move battle HUD. With
letter-based Xbox bindings, A appears below, B right, X left and Y above,
without changing which move each button activates. Original behavior is the
default. This feature does not change rendering, shaders or synchronization.

## Prepare the local resource and build (Windows)

Use the project's existing [Windows build prerequisites](BUILD-windows.md),
Python 3.10+ and a working installation containing `extracted/romfs.bin` and
`zaprompts/_internal/ooz.dll`. Use 64-bit native Windows Python so ctypes can load
the existing Windows DLL. From the source checkout, with clang on PATH:

```sh
python -B tools/test_xbox_attack_layout.py
python -B tools/test_settings.py
python -B tools/za/xbox_attack_layout.py 'C:/Games/PokeZA' --prepare
python -B tools/test_xbox_attack_layout.py --installation 'C:/Games/PokeZA'
make -j4 GENOPT='-O1 -DGUEST_VOLATILE' GENARCH=-march=x86-64-v3 build/lasrecomp.exe
```

Replace the example installation path. The generator reads the player's game
and writes only into the source checkout: `assets/xbox_attack_layout/` and
`runtime/xbox_attack_resource.h`. Generate before compilation. The assets are
ignored by Git and must never be committed, attached or redistributed. The
header contains only resource hashes, sizes and offsets. The Xbox changes do
not alter the game-library inputs or flags; preserve the flags required by
your own installation's DLL and keep the compatibility check enabled.

## Install and enable

With the game stopped, back up the current executable in a separate experimental
installation using a unique name, then copy `build/lasrecomp.exe` there as
`lasrecomp.exe`. Copy the contents of `assets/xbox_attack_layout/` into
`<installation>/mods/_XboxAttackLayout/`, producing:

```text
mods/_XboxAttackLayout/resource.json
mods/_XboxAttackLayout/romfs/ui/data/btl_app/btl_app_waza_00.arc
```

Leave original game files, saves, settings and caches intact. The leading
underscore keeps this folder out of normal Mods scanning; only the new setting
mounts it. Open **Settings > Controls**, leave **Battle moves by position** OFF,
enable **Xbox attack layout**, and restart the game. Default is OFF. To restore
the original HUD, turn it OFF and restart. Restart after changing face-button
bindings or either layout option. No hidden input remapping is performed.

## Presentation, not controls

The existing `Battle moves by position` option in `runtime/hle/input.c`
temporarily changes input bindings while ZL is held. This new option does not
change that code. `runtime/services/fs.c` reads `g_bind_pad` at RomFS mount and
chooses where each logical move container should appear. With Switch A/B/X/Y
bound to Xbox physical south/east/west/north, A is below, B right, X left and Y
above. All 24 permutations of four distinct face bindings are supported.

The exact catalog association was verified locally:

```text
ui/data/btl_app/btl_app_waza_00.arc
  -> arc/uidatabtl_appbtl_app_waza_00.arc.trpak
  -> SARC member blyt/btl_app_waza_00.bflyt
```

| Original parent | Position | Child template |
|---|---|---|
| L_waza_00 | top | btl_app_waza_parts_01 |
| L_waza_01 | right | btl_app_waza_parts_00 |
| L_waza_02 | left | btl_app_waza_parts_02 |
| L_waza_03 | bottom | btl_app_waza_parts_01, text override below |

The inspected original main image constructs `L_waza_%02d` at `0x9841c8` and
`0xa06b40`, and `L_waza_%02d/%s` at `0x2a154c8` and other locations, hashes the
paths and queries the UI. These are original image offsets, not host addresses.
Reassigning the parent names routes those updates to each destination template.
For letter-based Xbox bindings, top/right/left/bottom become 02/03/00/01.

Only four final name digits change in memory. The archive's geometry, child
layouts, text/icon resources, state/selection effects, colors and animations are
otherwise byte-identical. Every renamed parent appears once; the inspected
BFLAN animations target the common parent or children, not these four names.
The different templates place their text outward, so simply swapping parent
coordinates would not preserve the intended layout.

## Resource handling

`tools/za/xbox_attack_layout.py` reuses the existing read-only RomFS/ONEPACK
readers through selected AST definitions from `prompt_textures.py`. It checks
the catalog's virtual-path-to-pack association, decompresses only the candidate
resource through the player's existing ooz library, inspects SARC/FLYT and
generates a private archive plus a source metadata header. The header contains
only hashes, sizes and offsets; it has no game payload.

`runtime/xbox_attack_layout.h` implements the pure CPU mapping and guarded
relabeling. `runtime/xbox_attack_resource.h` holds generated metadata.
`xbox_layer_prepare()` verifies the source packed bytes and full local archive
before changing any byte, rejects unsupported bindings and position-input mode,
and adds a hidden opt-in RomFS overlay. The existing filesystem serves its
verified in-memory copy. No original resource is written and no graphics or
input command is intercepted. Original behavior is the default.

The runtime checks are compatibility guards, not claims of support for arbitrary
game versions. The included metadata was observed with main build ID
`7222E13ECF6ADB32`. The generator rewrites it locally before compilation; new
layouts require review. The layer takes priority for this one UI archive.

## Testing and limits

Run `python -B tools/test_xbox_attack_layout.py` for game-independent tests.
Prepare the player's resource and add `--installation <path>` for real archive
and filesystem preparation tests. The latter use temporary copies; the original
RomFS is read-only. Tests without a private resource explicitly report those
checks as SKIP. Resource preparation has been tested on Windows; other platforms
and game versions are not claimed to be covered.

Tests exercise the real C transform: 24 valid permutations, 1272 invalid binding
combinations, unchanged input, short/corrupted resources, and 12 setting
roundtrips. With a local fixture they check every archive byte for all 24
permutations, SARC/FLYT identity and animation-reference preconditions, source
guards, default OFF, conflicting mode, invalid bindings/ranges and fallback.

The contributor reports successful in-game testing of the development feature.
Automated source validation does not launch the game. The ordinary
four-move HUD is covered; special finishing/last-move interfaces remain original.
Face bindings and layout settings take effect after restarting. Full coverage
of moves, languages, animations, controllers and future game versions remains
outside the reported test scope. This UI feature is not a GPU-fault correction.

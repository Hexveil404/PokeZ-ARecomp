# ooz in ZA Recomp

The Kraken / Mermaid / Selkie / Leviathan / BitKnit / LZNA decompressor by Powzix, from
https://github.com/powzix/ooz (commit 05038060aa68f9187ae9923b2388ca8db40e58d1), unchanged: `bitknit.cpp`,
`kraken.cpp`, `lzna.cpp`, `stdafx.*`, `targetver.h`, `README.md`. **GNU GPL version 3 or later** (`COPYING`).
`ooz_lib.cpp` is ours: one C export so Python can call it (GPLv3 too). `oodec.cpp` is ours too (GPLv3): a small
program, `zaprompts/oodec`, that decodes one block. The game runs its own decoder for its packs; when that fails on a
block (#248), ZA Recomp runs oodec as a separate process on the same bytes and hands the game the result - it never
links or loads ooz.

It is used only by the **zaprompts** helper (`tools/za/prompt_textures.py` frozen by PyInstaller, shipped as
`zaprompts/` in the Windows release with `ooz.dll` next to it): the start screen runs it once on the player's own game
files to read Pokémon Legends Z-A's Oodle-compressed packs and redraw its button icons for the keyboard, Xbox and
PlayStation controllers. zaprompts is a separate program that the start screen runs as its own process; ZA Recomp
itself (MIT, `LICENSE`) neither links nor loads ooz. zaprompts with ooz inside is distributed under the GPLv3, and its
complete source is this repository (`tools/za/prompt_textures.py`, `tools/zaprompts.spec`, this folder).

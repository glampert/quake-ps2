---
paths:
  - "src/ps2/save/**"
  - "src/quake/host_cmd.c"
  - "src/quake/menu.c"
---

# Save games and config.cfg

The README's "Save games" section and `save/save_system.h` give the overview. Decisions and
facts behind them:

## Design

- **The user's decisions, from the Quake II port:** saves on the memory card in MEMORY CARD slot
  1; a slot written A/B so a failed write never loses the save before it; deflated with miniz;
  a 3D icon of the quad damage; `ps2_savedevice` (`host`/`mc`, default `host`) honoured only
  when `com_gamedir` starts with `host:`, the card always on a console.
- **Quake 1 is simpler than Quake II:** a save is one text `.sav` (`Host_Savegame_f`'s
  fprintfs), and nothing persists between levels, so Quake II's RAM working set
  (`save/current/`, `working_set.cpp`) is gone. Under `ps2_savedevice host` saves are plain
  QuakeSpasm `.sav` files in `<gamedir>`, byte for byte what a desktop QuakeSpasm writes; only
  the card gets archives.
- **Engine hooks** (`engine_hooks.h`, all tagged): `PS2_SaveOpenWrite`/`PS2_SaveCloseWrite` in
  `Host_Savegame_f` (the close returns failure, which QuakeSpasm's `fclose` never reported),
  `PS2_SaveLoadText` in `Host_Loadgame_f`, `PS2_SaveReadComment` in `M_ScanSaves`. The hooks
  print the "Saving game to"/"Loading game from" lines, so they name the card. `PS2_Config*`
  in `Host_WriteConfiguration` and `Cmd_Exec_f`. `M_Options_Key` writes the config on leaving
  the options menu.
- The menus probe the card once per frame (`host_framecount`): `M_ScanSaves` reads twenty
  slots in a row, and a missing card would otherwise print its error twenty times.
- miniz is the `src/tools/miniz` submodule (pinned 3.1.2), compiled `-O3` in both configs.
  `save/miniz_cfg/miniz_export.h` stands in for the CMake-generated header and carries
  `TDEFL_LESS_MEMORY`, which changes struct sizes. Everything must agree on it.

## Format

- A slot is `<slot>_a.q1s` / `<slot>_b.q1s` in `mc0:/Q1PS2/`, written to the *other* file,
  after which the older one is deleted. The newest valid sequence wins. There is no rename on
  rom0 MCSERV. Slot names are 1-24 characters of `[A-Za-z0-9_-]` (a card entry is 32 bytes).
- A 96-byte header (magic `"Q1PS"`, version 1, sequence, raw and packed sizes and CRCs, the
  39-character menu comment), then the raw-deflated `.sav`. Header CRC, packed CRC, raw CRC
  and the file length are all checked before the text reaches the game. Compatibility beyond
  that is the `.sav`'s own `SAVEGAME_VERSION`, which `Host_Loadgame_f` checks.
- Loading inflates the whole text into one malloc block (QuakeSpasm frees it), straight into
  the output with `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`: no dictionary of its own.

## Measured (PCSX2, debug, 2026-10-08)

e1m1's save: 83,433 bytes of text → 11,350 bytes on the card (7.4x). The first card save of a
session took 1.57 s, including starting MCMAN/MCSERV and writing the 48 KB icon; the next 0.67 s.
A host-file save takes ~0.2 s. Writing holds the 164 KB compressor (`MemTag::SaveData`) for its
duration.

## Icon

- The quad damage pickup, `progs/quaddama.mdl` (in the shareware pak), frame 0 and skin 0,
  parsed straight from the MDL with every offset checked (`mc_icon.cpp`). Its seam vertices
  take the back half of the skin on back-facing triangles, as `GL_MakeAliasModelDisplayLists`
  does. The skin goes through `d_8to24table` and is stretched bilinearly over the 128x128
  texture. 106 triangles, both windings: 636 vertices, 48,096 bytes. Without the model, the
  icon is a double-sided square.
- Model space maps to icon space as X ← Y, Y ← −Z, Z ← −X, normals from `r_avertexnormals`.
  The longest bounding-box side is 3.2, standing on Y = 0.
- The BIOS browser reads the community-documented `.ico` layout (type 0x07, 4.12 fixed point,
  Y down, 0x80 colours) and **ignores the texture alpha bit**.
- `EnsureSaveIcons` rewrites icon files that differ from this build's, once per card per
  session. Checked by rendering the card's `icon.ico` in software (scratchpad script): the Q
  emblem, upright, its skin mapped across the seam.

## config.cfg policy (`save_api.cpp`)

The engine still builds the text (`Key_WriteBindings`, `Cvar_WriteVariables`, into an
`open_memstream`); where it goes and where it comes from is the backend's.

- **Write:** `<gamedir>/config.cfg` only when running from `host:` (**never** to `mass:` or
  `pfs0:`), plus `mc0:/Q1PS2/config.cfg` whenever saves go to the card. The card write is
  skipped when unchanged. QuakeSpasm sorts its cvars, so an unchanged setup writes the same bytes.
- **Read:** emulator = the host file first, the card as fallback. Console = **the card first**,
  then the game data's `id1/config.cfg`. `exec config.cfg` and the early reads
  (`ReadConfigCvars`, for `ps2_fb_16bit` at `VID_Init` and `in_keyboard` at `IN_Init`) follow
  the same order. On a console that starts the card driver during `VID_Init`, ahead of the pad's;
  `LoadRomModuleOnce` keeps SIO2MAN to one copy, and the pad still comes up locked in analog
  mode with its motors.
- An archived cvar that a run never registers is dropped from the file (id's behaviour): a
  `set` creates it with no flags. So **register every `CVAR_ARCHIVE` cvar in every build**,
  outside any `PS2_QUAKE_DEBUG`/`PS2_QUAKE_PROFILE` gate, even if nothing reads it there.
- The console branch is only host-tested: this setup can't boot from `mass:`.

## Testing

- Scripted from `id1/autoexec.cfg`: `ps2_savedevice mc`, `map e1m1`, waits, `save s3` (twice,
  for the A/B swap), `ps2_saveinfo`, `menu_load` with a `screenshot`, `load s3`. Config: `quit`
  with `ps2_savedevice mc` writes both files; then boot with the host `config.cfg` moved away
  and it is read off the card. **Back up `memcards/Mcd001.ps2`, `id1/config.cfg` and
  `history.txt` first and restore them after**: the card holds the user's Quake II saves too.
- The PCSX2 card (`memcards/Mcd001.ps2`) is a raw 8 MB image: 528-byte pages (512 bytes + 16
  bytes of ECC), the superblock "Sony PS2 Memory Card Format", the FAT via `ifc_list`, and
  512-byte directory entries with the name at 0x40. A ~90-line Python reader lists and extracts
  what the game wrote; Python's `zlib.decompress(payload, -15)` inflates an archive's payload.
- `Slot1_Enable = false` in PCSX2.ini simulates "no card".
- The browser's view of the icon needs the user's eyes (boot PCSX2 with `-bios`).
- Memory card and FILEIO traps (MCSERV, `mcInit`, IOP flags, `remove()`→`mkdir`) are in
  `ps2-platform.md`.

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
  the options menu. The Options page's Save Device row reads `PS2_SaveDeviceName` and steps
  with `PS2_SaveDeviceToggle`, which does nothing off `host:`.
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

## The memory card notice (`card_notice.cpp`)

- **The user's request (2026-10-20):** while a save goes to the card or comes back off it, the
  notice PS2 games show: "Data is being saved to the Memory Card in slot 1. Do not remove the
  Memory Card or reset/switch off the console." ("loaded from" for a load), with the music faded
  to silence before the card is touched. It is Quake's message box (`M_DrawTextBox`, the quit
  box's), gold for what is happening and white for the warning, centred over the faded game, or
  under the plaque on the loading screen.
- **Order:** probe the card (none, or unformatted: no notice, and the access fails with the reason
  as before); stop the rumble; draw a frame with the notice and show it at once; fade the music out
  (`audio::HoldMusic`); then the card. Afterwards the notice stays until it has been up a second
  (`kMinShownMsec`), so a quick read can still be read, and the music fades back in. The notice
  goes up before the fade, not after: the other way round the game would sit frozen for half a
  second with nothing to say why.
- **Where:** a save puts it up in `PS2_SaveOpenWrite` and takes it down in `PS2_SaveCloseWrite`,
  after `StoreSlot`. Not in the close alone: the engine writing the game out between the two (the
  `fprintf`s, deflated as they go) holds the mixer up for over 400 ms in a debug build, and the
  music stopped dead there, before any fade (PCSX2, 2026-10-20). Nothing between open and close
  can `Host_Error`, so the close always comes. A console `load` puts it up in `PS2_SaveLoadText`,
  around `RestoreSlot`. The load menu has to start sooner: `M_Load_Key` raises the loading plaque,
  whose `S_StopAllSounds` would cut the music off, before `load` runs. So it calls
  `PS2_SaveLoadStarting` first, and `PS2_SaveLoadText` finds the notice up, on the loading screen,
  and takes it down once the card is read.
- Saves on host files (`ps2_savedevice host`) get neither the notice nor the fade: they stay
  QuakeSpasm's.
- **A failed load takes the load menu's plaque down** (`EndPlaqueOfFailedLoad`). QuakeSpasm's
  `Host_Loadgame_f` returns with it up, until its timeout (150 s here) over the running game, and
  here a card pulled after the menu listed the slots is enough. It skips `SCR_EndLoadingPlaque`'s
  `Con_ClearNotify`, so the reason stays on the screen.
- **config.cfg too (the user's request, 2026-10-20):** the card's copy is written under the notice
  (leaving the options menu, quit, `game`) and read under it. The boot's read happens in
  `VID_Init`, before the renderer exists, so the notice is drawn over the boot splash
  (`splash::ShowImage`): the box and lines composed on the EE into one RGBA texture from
  `gfx/box_*.lmp` and gfx.wad's conchars through the built-in palette, drawn at twice its size,
  centred, its see-through texels left out so the plaque shows through them. Between `gs::Init`
  and `SCR_Init` nothing can be drawn at all (the notice is then `Where::Nowhere`, and isn't held
  to its minimum), and nothing reads the card there. Whether the card has a config to read is
  found from its directory first, without the notice: a card the game never wrote to would show
  "Data is being loaded" at every boot, for its second, with nothing to load.
- **The main menu's Quit used to write config.cfg from inside a frame:** QuakeSpasm's "Quit now!"
  runs `Host_Quit_f` from `M_Draw`, inside `SCR_UpdateScreen`, and a notice can't draw a frame
  inside another (`rs::BeginFrame` asserts). It now queues `quit` for the top of the next frame,
  as `test_quit.cpp` already did. The notice still refuses to draw inside a frame. On the way
  out `Host_Shutdown` sets `scr_disabled_for_loading` to keep `Con_Printf` from drawing; the
  notice lifts it for its one frame.
- **Not covered:** the menus' slot listing (`M_ScanSaves`, twenty headers read when the Save or
  Load menu opens) touches the card with no notice.
- **Checked in PCSX2 (debug, e1m1 with track 6 playing, 2026-10-20):** console saves had the
  notice up 1.30-1.43 s (about 0.61 s of it the music's hold, then the game written and stored);
  a console load and the load menu's (through `M_Load_Key`) were held to the 1 s minimum, the
  menu's on the loading screen once its plaque was up; screenshots taken as the notice went up
  and came down matched, so it stayed on screen throughout. With no card (`Slot1_Enable =
  false`) neither a save nor the menu's load put the notice up or held the music, and the failed
  load's plaque came down with "No memory card in MEMORY CARD slot 1." on the screen.
- **config.cfg, checked the same way (2026-10-20):** booted with no host `config.cfg`, the card's
  was read under the notice on the splash, once (`IN_Init` and quake.rc's exec took the copy),
  and the frame-0 shot matched an independent rebuild of the splash and the box from the pak to
  the pixel, plaque included (11,264 plaque pixels, 64,512 in the box). A card without the
  game's folder (a copy of `Mcd002.ps2` as slot 1) booted with no notice and the bare splash.
  Leaving the options menu wrote the card's copy under the notice, over the options menu, with
  the music held; leaving it again unchanged touched nothing; and the main menu's quit after a
  change wrote it under the notice, over the console, on its way out.

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
  skipped when unchanged.
- **The card's copy (`CardConfig`, 2026-10-20):** what the card's config.cfg holds is kept in RAM
  (about 3 KB): read off the card by the boot's first read, and updated by every write. The boot's
  later reads and any later `exec config.cfg` are answered from it, and a config equal to it isn't
  written, nor the card even looked at: each would put the card notice up for nothing (the old
  check read the card's copy back on every options exit). A probe that finds another card, or
  none, makes it stale (`Device::Generation`), and then a write reads the card's file to compare
  first, as before. A card swapped with no probe since and no setting changed keeps its own
  config.
- **The text is deterministic**, which is what lets the repo track `id1/config.cfg`: binds in
  key-number order, then the archived cvars in their name-sorted list, each as the string it was
  set to, then `vid_restart`. A config in gives the same bytes out, from the debug and the
  release build alike (2026-10-12). What the builds differ in is the defaults of the debug
  overlays' two switches (`ps2_debug_overlays` and `ps2_show_fps`: on in debug, off in release;
  the panels' own `ps2_show_*` are on in both since 2026-10-14), which only show in a config
  written without those lines. Code that sets an archived cvar for its own
  purposes puts it back before a quit (the perf capture does, `perf_run.cpp`).
- **Read:** emulator = the host file first, the card as fallback. Console = **the card first**,
  then the game data's `id1/config.cfg`. `exec config.cfg` and the early reads
  (`ReadConfigCvars`, for `ps2_fb_16bit` at `VID_Init` and `in_keyboard` at `IN_Init`) follow
  the same order, and only the first touches the card (see the card's copy above). On a console that starts the card driver during `VID_Init`, ahead of the pad's;
  `iop_boot.cpp` keeps SIO2MAN to one copy (`StartMemoryCardDriver`, `StartPadDriver`), and the
  pad still comes up locked in analog mode with its motors.
- An archived cvar that a run never registers is dropped from the file (id's behaviour): a
  `set` creates it with no flags. So **register every `CVAR_ARCHIVE` cvar in every build**,
  outside any `PS2_QUAKE_DEBUG`/`PS2_QUAKE_PROFILE` gate, even if nothing reads it there.
- The console branch's config read has run in PCSX2, booted off a USB image with `HostFs` off
  (2026-10-13, see testing-pcsx2.md): the card first, then the drive's `id1/config.cfg`. Saving
  on that path is still untested.

## Testing

- Scripted from `id1/autoexec.cfg`: `ps2_savedevice mc`, `map e1m1`, waits, `save s3` (twice,
  for the A/B swap), `ps2_saveinfo`, `menu_load` with a `screenshot`, `load s3`. Config: `quit`
  with `ps2_savedevice mc` writes both files; then boot with the host `config.cfg` moved away
  and it is read off the card. **Back up `memcards/Mcd001.ps2` and `id1/config.cfg` first and
  restore them after**: the card holds the user's Quake II saves too.
- The PCSX2 card (`memcards/Mcd001.ps2`) is a raw 8 MB image: 528-byte pages (512 bytes + 16
  bytes of ECC), the superblock "Sony PS2 Memory Card Format", the FAT via `ifc_list`, and
  512-byte directory entries with the name at 0x40. A ~90-line Python reader lists and extracts
  what the game wrote; Python's `zlib.decompress(payload, -15)` inflates an archive's payload.
- `Slot1_Enable = false` in PCSX2.ini simulates "no card".
- The browser's view of the icon needs the user's eyes (boot PCSX2 with `-bios`).
- Memory card and FILEIO traps (MCSERV, `mcInit`, IOP flags, `remove()`→`mkdir`) are in
  `ps2-platform.md`.

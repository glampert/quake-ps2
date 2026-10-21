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
  a 3D icon of the quad damage.
- **The user's decisions on other devices (2026-10-21):** `ps2_savedevice` is `mc` (the default),
  or the boot drive's `host`, `usb` or `hdd`; a value naming a drive the game didn't boot from
  means the card, and the Options row steps between the card and the boot drive. Every device
  holds the same compressed slot archives, on a drive in `<gamedir>/save/` (as the Quake II port's
  `baseq2/save/`), and every device gets the save notice. Loads read the selected device only.
  config.cfg lives with the saves (below). A missing card or a failed save is told to the player
  (below).
- **Quake 1 is simpler than Quake II:** a save is one text `.sav` (`Host_Savegame_f`'s
  fprintfs), and nothing persists between levels, so Quake II's RAM working set
  (`save/current/`, `working_set.cpp`) is gone. Plain QuakeSpasm `.sav` files on the host were
  the default until 2026-10-21; now no device holds one, and an old one in `id1/` isn't listed.
- **The devices:** `memcard.cpp` (libmc) and `drive.cpp`, the Quake II port's host-file Device
  over stdio, for all three drives: newlib sends `host:` to the ROM FILEIO and `mass:`/`pfs0:` to
  fileXio. Its kind comes from `com_gamedir`'s prefix. After each file written or deleted it calls
  `ps2::sys::SyncGameDataDevice` (the hard disk's PFS keeps metadata cached otherwise); on an
  unpatched host: FILEIO a `remove()` leaves a directory, which it removes. It asks no free space
  (no driver here says): a full drive fails the write.
- **Engine hooks** (`engine_hooks.h`, all tagged): `PS2_SaveOpenWrite`/`PS2_SaveCloseWrite` in
  `Host_Savegame_f` (the close returns failure, which QuakeSpasm's `fclose` never reported),
  `PS2_SaveLoadText` in `Host_Loadgame_f`, `PS2_SaveReadComment` in `M_ScanSaves`. The hooks
  print the "Saving game to"/"Loading game from" lines, so they name the device. `PS2_Config*`
  in `Host_WriteConfiguration` and `Cmd_Exec_f`. `M_Options_Key` writes the config on leaving
  the options menu. The Options page's Save Device row reads `PS2_SaveDeviceName` and steps
  with `PS2_SaveDeviceToggle`. `M_Save_Draw`/`M_Load_Draw` call `PS2_SaveMenuDrawProblem`, and
  `M_Save_Key`/`M_Load_Key` ask `PS2_SaveMenuProblem` before taking a slot.
- The menus probe the save device once per frame (`host_framecount`): `M_ScanSaves` reads twenty
  slots in a row, and a missing card would otherwise print its error twenty times.
- miniz is the `src/tools/miniz` submodule (pinned 3.1.2), compiled `-O3` in both configs.
  `save/miniz_cfg/miniz_export.h` stands in for the CMake-generated header and carries
  `TDEFL_LESS_MEMORY`, which changes struct sizes. Everything must agree on it.

## Format

- A slot is `<slot>_a.q1s` / `<slot>_b.q1s` in `mc0:/Q1PS2/` on the card and `<gamedir>/save/` on
  a drive, written to the *other* file, after which the older one is deleted. The newest valid
  sequence wins. There is no rename on rom0 MCSERV; FAT and PFS have one, but the drives keep
  the card's scheme. Slot names are 1-24 characters of `[A-Za-z0-9_-]` on every device (a card
  entry is 32 bytes).
- A 96-byte header (magic `"Q1PS"`, version 1, sequence, raw and packed sizes and CRCs, the
  39-character menu comment), then the raw-deflated `.sav`. Header CRC, packed CRC, raw CRC
  and the file length are all checked before the text reaches the game. Compatibility beyond
  that is the `.sav`'s own `SAVEGAME_VERSION`, which `Host_Loadgame_f` checks.
- Loading inflates the whole text into one malloc block (QuakeSpasm frees it), straight into
  the output with `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`: no dictionary of its own.

## The save notice (`save_notice.cpp`)

- **The user's request (2026-10-20):** while a save goes to the card or comes back off it, the
  notice PS2 games show: "Data is being saved to the Memory Card in slot 1. Do not remove the
  Memory Card or reset/switch off the console." ("loaded from" for a load), with the music faded
  to silence before the card is touched. It is Quake's message box (`M_DrawTextBox`, the quit
  box's), gold for what is happening and white for the warning, centred over the faded game, or
  under the plaque on the loading screen. **Every save device gets it (the user's request,
  2026-10-21)**, named in its second line ("USB drive.", "hard disk drive.", "host filesystem.")
  with its own warning ("Do not remove the USB drive"; "Do not reset or switch off the console."
  for the hard disk and the host); the box is sized for the longest line any device has, so it is
  the same box. It was the card notice (`card_notice.cpp`, `BeginCardNotice`) until then.
- **Every read and write of the card's data goes under it (the user's rule, 2026-10-21):** saves,
  loads, the menus' slot scan, `ps2_saveinfo`, config.cfg's reads and writes, the icon files.
  `memcard.cpp` asserts it at each method that touches the data (`EnsureSaveDir`, `List`,
  `FileSize`, `Open`, `Read`, `Write`, `Close`, `Delete`: "Memory card data accessed outside the
  save notice!"), so a debug build stops at any new path that forgets, and `drive.cpp` asserts the
  same of a drive's save directory. Only `Probe` (`mcGetInfo`: whether a card is in, and
  formatted; the first one also starts MCMAN and MCSERV) goes without it, as it decides whether
  there is a card to put the notice up for. A drive's config.cfg goes through stdio, not the
  Device: written under the notice when it is the save device's copy, read at boot without one
  (it is read before the save device is known, as a game data file, and in milliseconds).
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
  and `SCR_Init` nothing can be drawn at all, and neither inside a frame: there `BeginSaveNotice`
  refuses (`Where::Nowhere`) and the device is left alone, so its I/O always has the notice on the
  screen. No path does that now; after a failed first read, `IN_Init`'s would, and quake.rc's
  `exec config.cfg` reads the card again where the notice can be drawn. The directory read that
  finds whether there is a config goes under the notice too, so a card the game never wrote to
  shows it at boot for its second, with nothing to load.
- **The menus' slot scan:** `M_ScanSaves` reads twenty slots' headers as the load and save menus
  open, under one notice put up by `PS2_SaveScanBegin` and taken down by `PS2_SaveScanEnd`, around
  its loop (twenty notices would take twenty seconds). A slot read outside a scan puts up one of
  its own (`PS2_SaveReadComment`).
- **The main menu's Quit used to write config.cfg from inside a frame:** QuakeSpasm's "Quit now!"
  runs `Host_Quit_f` from `M_Draw`, inside `SCR_UpdateScreen`, and a notice can't draw a frame
  inside another (`rs::BeginFrame` asserts). It now queues `quit` for the top of the next frame,
  as `test_quit.cpp` already did. The notice still refuses to draw inside a frame. On the way
  out `Host_Shutdown` sets `scr_disabled_for_loading` to keep `Con_Printf` from drawing; the
  notice lifts it for its one frame.
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

## Telling the player (the user's request, 2026-10-21)

- **The load and save menus** look at the save device afresh as they open (`PS2_SaveScanBegin`
  probes, `ClearError` first), and when it can't be used draw a box over their slots
  (`PS2_SaveMenuDrawProblem`): the reason in white ("No memory card in MEMORY CARD slot 1."), then
  "To use the <other device> instead, change Save Device in the Options menu." in gold. The other
  device is the boot drive from the card, the card from a drive. Cross on a slot meanwhile scans
  again instead (a card put in since counts) and doesn't try the slot.
- **A save or a load that fails** says so in a message box (`ReportFailure` through
  `SCR_ModalMessage`, so it waits for a button and the frame is shown at once): "Couldn't save the
  game." or "Couldn't load the game.", the reason `SetError` recorded, the same advice, "Press
  Cross to continue." It comes up after the save notice is down, and for a load after the load
  menu's plaque, so the game shows behind it. QuakeSpasm's own "ERROR: couldn't open." still goes
  to the console. config.cfg's failures stay on the console: they come on quitting and on every
  options exit.
- **The text is wrapped at 38 characters** (`AppendWrapped`), the most a box on the 320-unit menu
  canvas holds inside its border; `SCR_DrawNotifyString`'s box is held to that width too.

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

- **It lives with the saves (the user's choice, 2026-10-21).** On a console: written to the save
  device, the card's `mc0:/Q1PS2/config.cfg` or the drive's `<gamedir>/config.cfg` (QuakeSpasm's
  own place, the file `make_usb_image.py` puts the tracked config in). Read at boot from the
  drive's when it says `ps2_savedevice` is that drive, the player's own then, and the card isn't
  touched; otherwise from the card's, the drive's standing in when the card has none. A switch
  from the drive back to the card writes the card's and rewrites the drive's once, so it stops
  naming the drive (`DriveConfigNamesDrive`); a switch to the drive needs nothing on the card,
  since the drive's wins. Rejected: the card first (its notice at every boot even for a player on
  the drive, and the card's old copy winning over a switch), the drive always first (the stick's
  shipped config over the player's card), the newest (no common clock between card, FAT and PFS).
- **Under the emulator nothing changed:** `<gamedir>/config.cfg` is always written and read first,
  the tracked file every run starts from, and the card's copy is written when saves go to the
  card and read when there is no host file. The host write goes under the notice when host files
  are the save device. Every drive write (the emulator's too) is skipped when the file holds the
  same bytes, as the card's is.
- **The card's copy (`CardConfig`, 2026-10-20):** what the card's config.cfg holds is kept in RAM
  (about 3 KB): read off the card by the boot's first read, and updated by every write. The boot's
  later reads and any later `exec config.cfg` are answered from it, and a config equal to it isn't
  written, nor the card even looked at: each would put the save notice up for nothing (the old
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
- **Read:** `exec config.cfg` and the early reads (`ReadConfigCvars`, for `ps2_fb_16bit` at
  `VID_Init` and `in_keyboard` at `IN_Init`) follow the same order (`ReadConfigMalloc`; a
  console's choice of the drive or the card is made at the first and kept, `ConfigFromDrive`),
  and only the first touches the card (see the card's copy above). On a console that reads the
  card starts its driver during `VID_Init`, ahead of the pad's; `iop_boot.cpp` keeps SIO2MAN to
  one copy (`StartMemoryCardDriver`, `StartPadDriver`), and the pad still comes up locked in
  analog mode with its motors. With no card the three reads each probe it, and say so, before the
  drive's stands in.
- An archived cvar that a run never registers is dropped from the file (id's behaviour): a
  `set` creates it with no flags. So **register every `CVAR_ARCHIVE` cvar in every build**,
  outside any `PS2_QUAKE_DEBUG`/`PS2_QUAKE_PROFILE` gate, even if nothing reads it there.
- **Checked in PCSX2 (debug scratch builds, 2026-10-21):** host files: two saves rotated
  `s0_a.q1s` to `s0_b.q1s` in `host:/id1/save/`, an archive in the card's format (inflated and
  CRC-checked on the host), loaded back; the saves, the scan, `ps2_saveinfo`, the load and the
  config write each under the notice naming the host filesystem. The card, as before. No card
  (`Slot1_Enable = false`): the save menu's box, Cross scanning again and staying, `save` and
  `load` each answered by the message box. A drive save that can't be written (`id1/save` made a
  file): "Could not write host:/id1/save/s3_a.q1s." in the box, with the card as the advice. A USB
  image with `HostFs` off and `ps2_savedevice "usb"`: saves, a load, the scan and the config in
  `mass0:/id1/`; the next boot took the drive's config and didn't touch the card; switching back
  wrote the card's and rewrote the drive's to "mc"; the boot after read the card's under the
  notice on the splash; with no card the save menu offered the USB drive. The hard disk is the
  same Device code with PFS's sync, unchecked: PCSX2's HDD image here holds only the Quake II
  port's partition, and nothing on this Mac writes PFS.

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
- **The drives:** host files from the scratch-directory recipe with `ps2_savedevice "host"` (the
  saves land in its `id1/save/`); a USB drive from testing-pcsx2.md's USB image recipe, its
  `id1/config.cfg` saying `"usb"`. Between runs attach the image read/write (`hdiutil attach
  -nobrowse -mountpoint <dir> -imagekey diskimage-class=CRawDiskImage <image>`) to change its
  `autoexec.cfg` and read what the run left, then detach it before PCSX2 starts. A `screenshot`
  goes to the image's `id1/` and takes seconds there. To make a drive save fail, put a file where
  `id1/save` goes.
- **The message box waits for a button**, which a script has none of: a scratch build's
  `SCR_ModalMessage` takes a `screenshot` after its `SCR_UpdateScreen` and gives itself a 2 s
  timeout when it has none (testing-pcsx2.md).
- The browser's view of the icon needs the user's eyes (boot PCSX2 with `-bios`).
- Memory card and FILEIO traps (MCSERV, `mcInit`, IOP flags, `remove()`→`mkdir`) are in
  `ps2-platform.md`.

# Testing: PCSX2, scripted sessions, host harnesses

## PCSX2 setup and logs

- App: `/Applications/PCSX2.app/Contents/MacOS/PCSX2`, launched as `PCSX2 -batch -elf <elf>`
  (`make run`). `host:` maps to the ELF's directory, and `make run` symlinks
  `build/<config>/id1` → the repo's `id1/`.
- `~/Library/Application Support/PCSX2/inis/PCSX2.ini` needs `[EmuCore] HostFs = true`
  (otherwise "No game data found"). Game stdout needs `[Logging] EnableIOPConsole = true`,
  because ps2sdk stdout goes through IOP fio, plus `EnableFileLogging = true` to land in
  `~/Library/Application Support/PCSX2/logs/emulog.txt`. Engine lines carry a `[Q1]` prefix,
  one per line however QuakeSpasm split the print, and Quake's console glyphs come out as
  ASCII (the separator bar as `-----`).
- **Edit PCSX2.ini only while PCSX2 is closed.** It rewrites the file on exit.
- Every launch overwrites `emulog.txt`. Copy it out before the next launch if you need it.
- **An `id1/pakN.pkz` (from `make pkz`) is loaded instead of `pakN.pak`.** The log says `Added
  packfile host:/id1/pak0.pkz (339 files, compressed)`. Since 2026-11-01 `make pkz` stores the
  demos (`PKZ_FLAGS`, `pakzip --store-ext .dem`), so they stream through the loader's ring: a pkz
  made before has them deflated, and `developer 1` then shows no `Read-ahead stream on demoN.dem`
  line at a demo's start. To test the pak path, move the pkz out of
  `id1/`, and don't leave a stale one behind after changing a pak: `make pkz` redoes a pkz only
  when its pak is newer. A pak has no such line (QuakeSpasm prints none).
- **An ELF path of 152 characters or more never reaches `main()`.** PCSX2 passes it to the BIOS
  as `host:<absolute path>`, and from 152 characters (that string's length) the BIOS's loader
  faults right after `# Restart Without Memory Clear Done.`: `TLB Miss, pc=0x80005558 addr=0x0`,
  then a run of them at `pc=0x0`. 151 boots (US BIOS v02.00, 2026-10-14). Scratch directories
  under the session scratchpad sit close to it: keep their names short. Not the `_request_end`
  flake below, which comes later, inside the game's file I/O.
- PCSX2 memory card slot 1: `~/Library/Application Support/PCSX2/memcards/Mcd001.ps2`.
  `Slot1_Enable = false` in the ini simulates "no card".
- `pcrtc_offsets = true` under `[EmuCore/GS]` makes PCSX2 move the picture as the GS DISPLAY
  registers say (`ps2_screen_x`/`ps2_screen_y`); off, the default, it never moves. `screenshot`
  reads VRAM, so it never shows the shift either way: `developer 1` logs each change (`GS: picture
  +8 pixels, -4 rows: DISPLAY DX 684, DY 42.`), and the rest wants eyes, on a TV above all.
- `[USB1] Type = hidkbd` attaches a host-passthrough USB keyboard. It reports itself as JIS,
  and its boot `Missing host mapping for QKey` warnings are harmless. It sends HID usage
  `0x34` for the host's `` ` `` key, never `0x35`. `in_keyboard` defaults to 1 in debug builds
  only, so a release build ignores it unless the config sets `in_keyboard "1"` (the tracked
  `id1/config.cfg` does). An idle keyboard is read four times a second, so the first key after
  a quiet spell can take up to 250 ms to land.
- `Pad: DS2 Config Finished ... VS: Normal - VL: Normal` in the log means the vibration motors
  were mapped (`padSetActAlign`). `AL: On - AB: Locked` in the same line means the pad took the
  DualShock (analog) mode `pad.cpp` asks for, locked so the Analog button can't drop it.

## Working with the user's machine

- Claude can't screenshot PCSX2 (`screencapture` is denied) or send it keystrokes (`osascript`
  is denied), **but the game can screenshot itself**: the `screenshot` command reads the last
  finished frame out of GS VRAM into `id1/spasmNNNN.tga` (the first free number). Script it
  from `autoexec.cfg` behind enough `wait`s for the scene to have drawn, then
  `sips -s format png id1/spasm0000.tga --out <scratchpad>/shot.png` and Read the PNG.
  Delete the TGAs afterwards. A 16-bit framebuffer reads back at 5 bits a channel (palette
  grey 31 comes back as 24). Outside what the game draws (the PS2 BIOS, PCSX2's own UI),
  anything visual still needs the user's eyes.
- A screenshot recipe that worked (2026-10-06), in `autoexec.cfg`: `alias w10 "wait;..."` (ten
  waits), `alias w100 "w10;..."`, then `playdemo demo1`, `w100`, `w100`, `screenshot` for the
  HUD in play. `menu_main`/`menu_options`/`menu_setup` plus `w10` and another `screenshot` add
  the menus, and `toggleconsole` the console. `ps2_debug_overlays 0` and `ps2_show_fps 0` keep
  the debug panels out of the shot. The commands run before `quake.rc`'s `startdemos`, which
  then finds a demo playing and leaves it.
- **Brightness and Contrast** (`gamma`, `contrast`) show in a shot's mean pixel value: the TGA's
  bytes past its 18-byte header, averaged in Python. Over demo1's start (dark) on 2026-10-22: 16.8
  at gamma 1, 62.9 at `gamma 0.5`, 87.7 with `contrast 2` as well, and the menus drawn at
  `gamma 0.6` read 51.3 against the demo behind them. A change applies at the next frame, so one
  `w10` before the `screenshot` is enough.
- Launching PCSX2 by hand on a release build needs `build/release/id1`, which `make release
  run` creates: `ln -s $PWD/id1 build/release/id1`.
- The user may be at the machine while a run is up, and may close it. Don't relaunch PCSX2
  after they close a run without a reason.
- **Other projects on this machine run PCSX2 too.** doom3-bfg-ps2's test runners launch batches,
  a new instance every 15-20 s, and refuse to start a run while any `pcsx2` process is up ("an
  emulator is already running"), so a run of ours in the middle of a batch fails their next test.
  `pgrep -fl PCSX2` before launching, and if theirs are cycling, wait for a gap: 90 s with neither
  a PCSX2 nor a `src/tools/scripts/*.py` runner up came after 9 minutes (2026-10-22).
- **The user can drive the pad** (a host controller bound to PCSX2's `[Pad1]`) and the
  passthrough keyboard, which Claude can't. With `in_debugkeys 1` in `autoexec.cfg` every press
  lands in the log (`pad: Cross down -> ABUTTON`, `keyboard: usage 0x04 down -> a`). Until the
  3D view exists, bind a spare button to `viewpos` (prints position and angles) to show the
  sticks moving the player, and start the test in a map (`map e1m1` in `autoexec.cfg`): over the
  attract demo the camera is the demo's. Ask the user to close the PCSX2 window at the end
  rather than quit from the menu (see below).
- Runs read and rewrite `id1/config.cfg` (QuakeSpasm writes it on quit). It is tracked, and an
  unchanged setup writes it back byte for byte (debug and release, checked 2026-10-12), so
  `git diff id1/config.cfg` after a run shows what the run changed. **Restore it with
  `git checkout id1/config.cfg` after a scripted run that set an archived cvar**, unless the change
  is the point. Check `git status` first: an uncommitted edit of the user's would be lost. Start
  every run of a comparison from the same config. The main menu's Quit exits at once, writing it:
  clean it up after a user's test that quit that way.

## Scripting a session

- `quake.rc` runs `exec default.cfg`, `exec config.cfg`, `exec autoexec.cfg`, then `stuffcmds`
  (the command line's `+cmd` arguments). So `id1/autoexec.cfg` (gitignored, a loose file next
  to the pak) is the place for a scripted session, e.g. `map e1m1`. `startdemos` comes after
  it and does nothing once a map or a demo is running. Delete the file after the run.
- `timedemo demo1` plays demo1 as fast as the engine goes and prints `N frames S seconds F
  fps`. With nothing rendered it measured 970 fps (about 1 ms of EE time per frame) on
  2026-10-06; PCSX2 doesn't model the EE cache, so treat it as a rough baseline.
- `path` lists the search path and `mods` the directories next to the ELF (on `host:` that
  is `build/<config>/`: `id1`, `irx`, `miniz`, `src`), which shows `opendir` works.
- `make run RUN_ARGS="..."` puts arguments on the game's command line through PCSX2's
  `-gameargs`, e.g. `RUN_ARGS="-heapsize 20480"`. PCSX2 passes them without a program name in
  `argv[0]`; `main.cpp` adds one, since QuakeSpasm reads options from `argv[1]` on. For runs
  that shouldn't overwrite `emulog.txt`, launch PCSX2 directly with `-logfile <path>`.
- **`+commands` do nothing with the shareware data.** `stuffcmds` reads the `cmdline` cvar,
  and `COM_CheckRegistered` only fills it when `gfx/pop.lmp` (the registered pak1) is found,
  as in id's Quake. `RUN_ARGS="+map e1m1"` boots into the demo loop. Use `id1/autoexec.cfg`.
- `-dedicated` doesn't work: QuakeSpasm stops with "Network not available!" when no driver but
  loopback comes up. A normal run's `map e1m1` already exercises the server, QuakeC and
  physics, with the client attached.
- **A scratch run's own setting replaces the tracked `config.cfg` line, rather than following
  it.** The boot-time reads (`ps2_video_mode` and `ps2_fb_16bit` at `VID_Init`, `in_keyboard` at
  `IN_Init`) stop once they have found every name they look for, so a second line further down
  is never read for them: `sed 's/^ps2_video_mode "auto"$/ps2_video_mode "pal"/'` into the copy
  works, `echo ... >>` doesn't (2026-10-13).
- Record the traps here as they turn up. The Quake II port's (a 128-char `COM_Parse` overrun,
  `wait`s queued across a map load) came from Quake II's code and may not apply.

## Tests

- **`ps2_testcube 1`** (debug builds) draws the VU1 test cube over every frame: the GS, VIF1,
  VU1 microprogram and VRAM upload smoke test. With a `screenshot` it checks all of that
  without anyone watching. Its variants are in [CVARS.md](../../CVARS.md).

- **`ps2_testaio 1`** (debug builds): the background loader's self-test (`tests/aio_test.cpp`).
  It stops the attract loop (`disconnect`, as the perf run does), then reads 12 files through the
  loader and through `COM_LoadMallocFile` and compares them, as the drive is and throttled to
  900 KB/s, with a main-thread read of `gfx/pause.lmp` timed every frame meanwhile; each line
  gives the source kind (loose, pak, pkz-stored, pkz-deflated), the size and the request-to-landed
  time, and the run ends in `AioTest: PASS`/`FAIL` and quits (`ps2_test_quit`). About 10 s in
  PCSX2 from the pkz (2026-11-01: 24 comparisons, 0 failures, `tag Aio holds 40.19 KB` after each
  pass, the inflate workspace). The pak kind needs a run without the pkz. It writes config.cfg on
  the way out: restore it.

- **A background sound load**, by script: `developer 1`, `map e1m1`, two `w100`, then `menu_main`,
  `w10`, `togglemenu`: the console prints `sound: sound/misc/menu2.wav loaded in the background,
  N ms after it was asked for` (14 ms, 2026-11-01). A sound the level precached never shows up,
  and nor does one asked for under the plaque or during the precache pass.

The rest are scripted runs:

- **Map cycle** (debug builds): `ps2_testmaps 1` in `autoexec.cfg`. It lists every level in the
  paks (the campaign in play order, then the rest alphabetically: pak1's dm1-dm6), names the
  campaign maps the data lacks, loads each for 8 s (`ps2_testmaps_dwell`), prints three
  `MapCycle [n/N] <map> <pak>` lines per map (hunk, cache, backend tags, heap peak, RAM free; the
  load's own peak; the texture heap's peak in the level, what the level needed of it as if it never
  evicted, with its evictions, and what every texture it loaded would take: `VRAM peak ... needed
  ... (N evictions) loaded ...`) and ends with the fixed pools' peaks against their sizes (textures,
  texmgr's records, alias models), the texture heap's peak and the level that set it, the most a
  level needed and how many levels had it evicting, and `MapCycle: done.`, then quits
  (`MapCycle: done - quitting now`), which writes a
  config: the card's in a scratch run, so restore it. With `ps2_test_quit 0` it stays up instead
  (`... the game stays up for the next test`), as every test does. `ps2_testmaps 2` is its perf
  pass (see the perf run below).
- **Running a test on chosen data without touching `id1/`:** copy `build/debug/quake.elf` into a
  scratch directory beside an `id1/` of symlinks (`pak0.pak` alone is the shareware data: id's
  1.06 releases ship the same pak0; add `pak1.pak` and `music` for the full game) and its own
  `autoexec.cfg`, then `PCSX2 -batch -logfile <log> -elf <dir>/quake.elf`: `host:` is the ELF's
  directory. With no `config.cfg` there, the game reads the memory card's, and nothing writes
  one unless the run quits. **A run that quits writes the card's config**, so an archived cvar
  one scratch run sets reaches the next: a walk test's `host_maxfps 20` held the following perf
  pass to 20 fps (2026-10-10). Restore the card before each run, not only at the end. The nine shareware maps took ~2 minutes, clean, on
  2026-10-08, and the full registered data's 38 took 5.5 minutes, clean, on 2026-10-09; the
  numbers are in [memory-budget.md](memory-budget.md).
- **The console's boot and storage path** (`iop_boot.cpp`, the log file, anything that reads or
  writes the game data's drive): `HostFs = false`, and a raw MBR/FAT32 disk image on USB port 2
  (`[USB2]` `Type = Msd`, `Msd_subtype = 0`, `Msd_ImagePathMsd = <absolute path>`), with `id1/`
  (the paks and an `autoexec.cfg`) at its root. `make_usb_image.py` builds one from the current
  builds and the repo's `id1/`; for chosen data, `hdiutil create -size 64m -fs 'MS-DOS FAT32'
  -layout MBRSPUD -volname Q1PS2 -srcfolder <dir holding id1/> -format UDTO -nospotlight
  <name>.cdr` (rename it `.img`; 64 MB holds the shareware pak0). Keep a pristine copy of the
  image, since a run writes to it. Launch the scratch ELF with `-elf` as
  usual: PCSX2 still passes it a `host:` path, so the boot searches the HDD first (`[DEV9/Hdd]`
  is off: ps2dev9 fails, 2 ms) and then USB. On 2026-10-13 the debug build found the data on
  `mass0:` 661 ms in (600 ms of it waiting for the drive), loaded e1m1 and took a screenshot. Put
  `ps2_logfile 1` in the image's `autoexec.cfg`, and read what the run left with `hdiutil attach
  -readonly -nobrowse -mountpoint <dir> -imagekey diskimage-class=CRawDiskImage <image>`, then
  `hdiutil detach <dir>`. Edit the ini only with PCSX2 closed, and restore it after.
- **Compressed packs**: a scratch `id1/` of pkz symlinks and the map cycle, in a debug build, which
  checks the CRC of every file it reads from a pkz. `make pkz PAK_DIR=<dir>` on a directory of pak
  symlinks writes the pkz beside them, outside `id1/`. On 2026-10-18 the 9 shareware levels loaded
  from `pak0.pkz` alone (`Playing shareware version.`, so the shareware check still passes) and
  the 38 registered ones from both, clean, and the USB image recipe above, with only the pkz on
  it, booted from `mass0:` and changed levels under the plaque. For `COM_Game_f`'s `PS2_PkzClose`:
  `-gameargs "-game <dir>"`, with a `<dir>/pak0.pkz` (pakzip of a small pak) holding an
  `autoexec.cfg` that execs a cfg from it and ends in `game id1`. id1's own `autoexec.cfg` then
  runs, with no loop, and `ps2_memstats` shows the mod's 1.1 KB of `Pack` gone. The switch
  writes the config, to the card too with `ps2_savedevice "mc"`: back the card up first.
- **The loading screen**: the plaque only goes up over a game, so `map e1m1`, `w100`, then
  `changelevel e1m2` and `screenshot` straight after it. Nothing draws while the plaque is up, so
  the shot is the last loading screen frame. Waits after the `changelevel` leave the client stuck
  at signon 0 (engine-c.md's `SV_SendReconnect` quirk), so end the script there. A build with
  `PS2_QUAKE_LOAD_TRACE` 1 (load_trace.h) traces the load into `quake.log`; on 2026-10-13 the
  e1m1 to e1m2 change drew 6 loading frames over 49 files and 2.8 MB in 1.8 s.
- **The boot splash**: nothing scripted runs before the first frame replaces it, so a scratch
  build queues the shot: `SCR_Init (); Cbuf_AddText ("screenshot\n"); Cbuf_Execute ();` in
  `Host_Init` (host.c). No frame has been presented then, so `screenshot` reads frame 0, where
  the splash is. Put host.c back and `touch` it afterwards. On 2026-10-13 the shot held the plaque
  64x288 at (288, 80) on the NTSC BIOS, every pixel its texel's colour at `kColorIntensity`
  through the 16-bit framebuffer, and was black with `ps2_video_mode pal` or `ps2_fb_16bit 0`
  in the config (see the trap above). The splash logs `Splash: the QUAKE plaque is up (NTSC,
  640x448) after 1 ms.` That the menus draw the ELF's plaque shows on a copy of pak0 rebuilt
  without `gfx/qplaque.lmp`: the menus drew the same pixels, where the build before stopped with
  `Draw_CachePic: failed to load gfx/qplaque.lmp`.
- **Perf run** (profile builds): `ps2_perftest 1` in `autoexec.cfg` plays the demos, and
  `ps2_testmaps 2` tours every level; the recipes are in [performance.md](performance.md). Both
  quit at the end, which writes `config.cfg`, unless `ps2_test_quit` is 0.
- **The save notice** (`save/save_notice.h`): `save` and `load` run from a script, but a
  `screenshot` after either catches the frame after the notice. A scratch build with
  `Cmd_ExecuteString("screenshot", src_command);` after `ShowFrame()` in `BeginSaveNotice` (only
  where it drew over the game or the loading screen: on the splash there is no renderer to read)
  and before `s_shown = false` in `EndSaveNotice` catches it as it goes up and as it comes down
  (the TGA write stalls the mixer ~0.4 s first, so time the hold on a build without it). The load
  menu's path (`M_Load_Key`, which puts the notice up before its plaque) wants a key press: a
  scratch command that calls `M_Menu_Load_f()`, sets `load_cursor` and calls `M_Load_Key(K_ENTER)`
  drives it (all three are extern in menu.c), with `loadable[load_cursor] = 1` to try a slot the
  card doesn't have. `M_Load_Key` appends its `load` to the command buffer, behind the rest of the
  script, so have the scratch command append whatever must run after the load. `developer 1`
  prints `Music: held, faded out over ...`, `Card notice: down after ... ms` and `Music: fading
  back in`. Back up the card first. A screenshot meant to show a failed load's notify lines must
  come within `con_notifytime` (3 s) of them: 200 frames of `wait` is too late. The results of
  2026-10-20 are in [save-games.md](save-games.md).
  - **config.cfg's notice:** the scratch-directory recipe without a `config.cfg` makes the boot
    read the card's (under the emulator the host file wins). Its notice is on the splash, which
    `screenshot` can't reach from inside `VID_Init` (no renderer yet): skip the scratch shots
    unless the notice is over the game or the loading screen, leave the splash's notice up (no
    `HideImage`), and take the shot right after `SCR_Init` in `Host_Init`, which reads frame 0
    while the config's layout is the default one. For the writes, a scratch command that calls
    `M_Menu_Options_f()` and `M_Options_Key(K_ESCAPE)` leaves the options menu as the pad does
    (`M_Options_Key` is extern in menu.c); change an archived cvar (`volume 0.6`) to have
    something to write, and `menu_quit` takes the main menu's quit path. A run that quits writes
    the card's config: back it up first. So does leaving the options menu, which under the
    emulator also writes the scratch directory's own `config.cfg`: delete it before the next boot
    test, or the boot reads it and never goes to the card.
  - **Every path at once, with the debug asserts on** (memcard.cpp stops at card I/O outside the
    notice): `ps2_saveinfo`, `menu_save` then `togglemenu`, `save`, `load`, an options exit, the
    load menu (the scratch command), `menu_quit`, after a boot that reads the card's config. A
    `screenshot` run from inside a command (the scratch shots inside the scan) retokenizes, so
    that command's `Cmd_Argv` is gone afterwards: read the arguments first.
- **The menus' pages and rows** (the main menu, About, Options, Controls, the yes/no questions):
  `menu_main`, `menu_options` and `menu_keys`, each with `w10` and a `screenshot`. The rest want a
  key press, through scratch commands (menu.c's functions and cursors are extern, but
  `keys_cursor` is static, so put them in menu.c): `M_Menu_Main_f()`, `m_main_cursor = 3` and
  `M_Main_Key(K_ENTER)` open the About page; `M_Menu_SinglePlayer_f()`,
  `m_singleplayer_cursor = 0` and `M_SinglePlayer_Key(K_ENTER)` over a running map ask "start a
  new game?", which waits for an answer forever, so give the scratch build's `SCR_ModalMessage`
  a `Cmd_ExecuteString("screenshot", src_command)` after its `SCR_UpdateScreen` and a 2 s timeout
  when it has none (the shot reads the frame drawn, presented or not: log `GL_EndRendering`'s
  `deferPresent` to see it shown). The Options rows step with `options_cursor` and
  `M_AdjustSliders`; Stick Look's queues `-mlook` behind the rest of the script (engine-c.md), so
  type it. Lookspring: `+lookup` for 20 frames, then `+forward` for 100, with `viewpos` after
  each. With `lookspring 1` the pitch went from -73 back to 0, and with 0 it stayed (2026-10-21).
  Video Options (`menu_video`) takes its keys in vid.cpp's `VideoMenuKey`, in an anonymous
  namespace: a scratch `Cmd_AddCommand` in `VID_Init` that passes it `Q_atoi(Cmd_Argv(1))` steps
  the page by key code (keys.h: 129 down, 130 left, 131 right, 13 Enter), the cursor starting on
  the first row, and typing a row's cvar prints what the step set.
  A scratch `config.cfg` with `ps2_savedevice "host"` keeps the quit's write off the card.
- **The debug page** (Options → Debug Options, or `menu_debug`): a script can't press the pad,
  but `menu_debug`, `w10` and a `screenshot` show it, and its rows are commands and cvars to run
  directly. The cursor blinks between conchars 12 and 13 four times a second, so a shot catches
  it only every other quarter second.
- **Movement check across builds** (for physics and collision changes): a script that, on a few
  maps after `god 1` and `notarget 1`, holds `+forward`, `+moveright`, `+back` and `+jump` for set
  `wait` counts with a `viewpos` after each, run on the old and the new ELF. Put `host_maxfps 20`
  in both: a build that runs faster integrates the moves in smaller steps and ends somewhere
  else, collision or not. The float collision change (2026-10-10) ended in the same places on
  e1m1 and e4m7 and within 5 units on e2m2.
- **Config write**: `quit` runs `Host_Shutdown`, which writes `id1/config.cfg` through
  `host:`. Then PCSX2 boots into the PS2 BIOS menu; stop it there.
- **Stop every PCSX2 you launch once its log is read.** A run stays open after the game halts,
  quits or idles, and the next launch starts another instance beside it.
- **Launch PCSX2 from a background shell with its stdout to `/dev/null`.** It prints the whole
  emulog there as well (5 MB over a map tour), which swamps the shell's own output. A watcher
  that polls `emulog.txt` for `FLOG#end` or `MapCycle: done` every 15 s, then kills the process,
  copies the log into `build/baselines/` and restores the card, ran both tours of 2026-10-22
  unattended (10.5 minutes each).

## Quiet map for renderer work

`nomonsters 1` before `map e1m1` (QuakeSpasm filters monsters at spawn), or `notarget`,
`god` and `noclip` once in. Unverified on the PS2 until the 3D phase.

## Crash triage

- Debug builds print an EE exception report (cause, EPC, BadVAddr, stack). Resolve addresses
  against **the same build's** `quake_unstripped.elf`:
  `mips64r5900el-ps2-elf-addr2line -f -C -e build/debug/quake_unstripped.elf <addr>` or
  `build/tools/symbolize < emulog.txt`.
- **A render pipeline hang** (debug builds: a GS wait that ran past a second) prints the dump
  the README describes, then stops with `Sys_Error: Render pipeline hang: ...`. To exercise it
  for real (not a zero timeout, whose registers show a finished DMA), emit a DIRECT block whose GIF
  tag promises more than it carries, e.g. `GIF_SET_TAG(0x7FFF, 1, 0, 0, GIF_FLG_IMAGE, 0)` plus
  one qword, with render_system.cpp's `OpenDirect`/`DirectCursor`/`SetDirectCursor`/`CloseDirect`
  on the Nth 3D frame, after the world pass (`rs::KickBuilt`) so MSCALs precede it. *(Q2)* PCSX2 wedges as hardware would:
  `D1_CHCR STR=1`, VIF1 `VGW=1` on a FLUSH, GIF `APATH=PATH2 OPH=1`, and the chain trail flags
  the packet. PCSX2 reads the GIF tag registers and VPU-STAT as 0; only a console fills those in.
- *(Q2)* **VU1 memory that repeats one output shape** throughout means a microprogram looped on a
  corrupt count, its output wrapping around all 1024 qwords, constants included. In a 2026-10-10
  console log five particle-sprite qwords filled all of it. Simulate the program's writes
  (start, stride, count, wrap at 1024) against the dumped memory: only TOP 8 with count 0x7FFF
  matched every qword, and it put the XGKICK on the qword the `GIF_TAG` registers held, with no
  EOP anywhere, so PATH1 never closed. The cause was the particle batch overrun
  (vu-microprograms.md); the dump now flags a particle count out of range.
- **Known flake, never game code:** a `TLB Miss` in `_request_end` (ps2sdk `sifrpc.c`,
  `SIF_CMD_RPC_END`) during `host:` file I/O, usually right after a pak file is opened. The
  signature is one to three `TLB Miss, pc=<same> addr=0x10|0x18 [load|store]` lines:
  `cd->hdr.pkt_addr` is already null, one RPC completed twice under PCSX2's faked IOP HostFs.
  A second face of it is a wild `pc` below `.text` (0x100000), e.g. `pc=0x82000 addr=0x0`,
  which is usually fatal. The pc moves per build, so resolve it:
  ```sh
  mips64r5900el-ps2-elf-objdump -d build/debug/quake_unstripped.elf > q1.dis
  L=$(grep -n "^  1ba87c:" q1.dis | cut -d: -f1)
  awk -v n="$L" 'NR<=n && /^[0-9a-f]+ </ {f=$0} NR==n {print f}' q1.dis | c++filt
  ```
  **Re-run before investigating.** On the Quake II port, across three identical 39-map
  cycles, one died, one was clean, and one logged it and still finished.

## Host harnesses (runtime logic only; `make` stays the compile check)

- **Renderer sources:** in the scratchpad, make a `fakeinc/` with four headers and compile
  with `-Ifakeinc -I<repo>/src` (quoted includes miss the source's own dir, so `-I` order
  decides): `ps2/common.h` (`MAX_QPATH`, `PS2_QUAKE_DEBUG`, `Con_Printf`/`Con_DPrintf`/
  `Sys_Error` decls, `PS2_Assert`/`PS2_AssertMsg`), `tamtypes.h`, `gs_psm.h` (copy the
  `GS_PSM_*` values), `draw_buffers.h` (a `texbuffer_t`). Don't fake `texture.h`/`vram.h`.
  `#include` the **.cpp** so the test can walk anonymous-namespace statics. Stub `Sys_Error`
  as a counter (fatal paths `return` after it) and make the assert stub `exit(1)`. Use clang
  with ASan/UBSan.
- **Client-side sources** compile against the *real* QuakeSpasm headers on host clang in C++
  mode. Make the fake `ps2/common.h` wrap `#include "quake/quakedef.h"` in `extern "C"`,
  define the globals the source touches, and fake only the hardware class.
- **Emulate the EE FPU** in any math harness: `1/sqrt(0)` must give FLT_MAX, not inf (see
  [ps2-platform.md](ps2-platform.md)). Otherwise target-only bugs won't reproduce.
- **ASan can't see an overrun that stays inside one object** (a member array reading into
  the next member). Heap-allocate the object under test, so writes past its last member hit
  the redzone, and poison what must not be read with `ASAN_POISON_MEMORY_REGION` from
  `<sanitizer/asan_interface.h>` (it handles a partial first granule), unpoisoning after the
  call. Give outputs exact-size heap buffers with canaries. Then prove the harness by seeding
  defects into a shadow copy of the header (`-I<mutant dir>` first): the `half_band.h`
  harness caught 7 of 7 (off-by-ones, an over-long memmove, a wrong tap, a 32-bit overflow).
- **The pkz reader** (fs/pkz_pack.cpp) runs on the host the same way. Fake `ps2/common.h`
  (`MAX_QPATH`/`MAX_OSPATH`, `Con_Printf`/`Con_SafePrintf`, `q_strlcpy`, a `[[noreturn]]`
  `Sys_Error` that longjmps when a test expects one, and a `ps2::heap` that checks every `Free`'s
  size and tag against its `Alloc`), `ps2/engine_hooks.h` (the compressed-pack section, copied
  out of the real one), `ps2/system/sys.h` (`OpenFileHandle` into a vector) and `tamtypes.h`.
  macOS has no `fopencookie`: define `cookie_io_functions_t`, and `#define fopencookie` and
  `setvbuf` to stand-ins (the latter declared in `std`) after `<cstdio>` and before including the
  `.cpp`; record the buffering each stream asks for, and call the cookie callbacks as newlib's
  unbuffered `fread` (the read for all that is left, until 0 or -1), `fseek` and `ftell` (a seek
  of 0 from `SEEK_CUR`) do. Link the miniz sources with the game's `save/miniz_cfg`. On
  2026-10-18 the harness read all 424 files of id's pak0.pkz and pak1.pkz whole through handles,
  and as streams with 1000 random reads and seeks each (623K checks, with and without the CRC
  checks, under ASan/UBSan). It did the same over synthetic packs with 4 and 32 KB blocks and
  zips from Info-ZIP (extra fields, a comment, directories) and Python's `zipfile`, and stopped on
  13 malformed packs with the right `Sys_Error`. Of a damaged pack's reads, the CRC check failed
  every one, saying so once each; without it, 4 of the 6 delivered the wrong bytes.
  It caught 14 of 14 seeded mutants (the whole-file fast path, a stored file's offset and seek,
  the CRC, the buffering, the restart index, the dictionary window, stale input, the wrap copy,
  the local extra field, a warning printed twice). One that turned Restart's `<` into `<=` passed,
  and is equivalent. macOS has no `timeout`: bound a mutant run with `perl -e 'alarm 600; exec
  @ARGV' ...`, and check the exit code isn't 127.
- Code with EE/VU0 inline asm can't run on the host. Use the standalone test ELF recipe in
  [performance.md](performance.md).

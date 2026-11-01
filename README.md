
# Quake port for the PlayStation 2

![Quake PS2](misc/screens/banner.jpg "Quake PS2")

## Overview

This is an unofficial fan-made port of id Software's Quake to the PlayStation 2 console. It derives
from [QuakeSpasm][link_quakespasm] 0.97.0, the modern, faithful Quake engine based on FitzQuake and
id's GPL source release: the engine, client, server and QuakeC virtual machine here are
QuakeSpasm's. id's original release notes for the Quake source code are in
[src/README.md](src/README.md).

Like the [Quake II port][link_q2_ps2] it builds on, it relies solely on the free
[PS2DEV SDK][link_ps2_dev], with no official Sony SDK and no proprietary libraries. The goal is a fully
functional and playable single-player Quake on the PS2, built entirely with freely available tools.

QuakeSpasm's C code under [src/quake/](src/quake/) is kept as close to untouched as possible; every change
is tagged `// [PS2_QUAKE]: <why>`. Everything specific to the console lives in [src/ps2/](src/ps2/) and is
written in modern C++ (C++20, no exceptions, no RTTI, warnings-as-errors). That backend comes from the
[Quake II port][link_q2_ps2]: its renderer, input, audio, memory and debugging code carry over, ported to
QuakeSpasm's interfaces.

### Status

**Playable from start to finish, on a real PS2 and in PCSX2.** The whole single-player game runs,
the shareware episode or all four episodes of the registered game, with everything working:

- **Loading** the game data from a USB drive or the internal HDD on a console, and from PCSX2's
  host filesystem (`host:`) in the emulator, from id's paks or from compressed ones that halve what
  a level load reads.
- **Saving** games and settings to the memory card, or to the USB drive or hard disk the game runs
  from.
- **The DualShock 2**, with both analog sticks and rumble. A USB keyboard works too, for the in-game console.
- **Sound effects and the CD soundtrack**, the music streamed from ripped CD tracks.
- **Everything on screen:** textured, lightmapped worlds with their fullbright texels, warping
  water and scrolling skies, monsters, items and the view weapon, sprites, particles, and the
  in-game console, menus and status bar.
- **NTSC and PAL**, at 60 or 50 Hz.

What is left is refinement, mostly performance: a few of the heaviest views still drop below
60 fps. Not supported: network play (single player only), GS fog (no id map uses it), and
skyboxes or other external textures.

---

## Getting started

### Prerequisites

1. **The ps2dev toolchain and PS2SDK.** Build/install from [ps2dev][link_ps2_dev]
   (`ps2toolchain` + `ps2sdk`). The build expects these on `PATH`:

   | Tool | Comes from | Typical location |
   | --- | --- | --- |
   | `mips64r5900el-ps2-elf-gcc` / `-g++` | ps2toolchain | `$PS2DEV/ee/bin` |
   | `openvcl` | openvcl | `$PS2DEV/bin` |
   | `dvp-as` | ps2toolchain (dvp) | `$PS2DEV/dvp/bin` |
   | `bin2c` | ps2sdk | `$PS2SDK/bin` |

   A `~/.zshenv` (or equivalent) along these lines is what the Makefile and the VSCode tasks expect:

   ```sh
   export PS2DEV=$HOME/ps2dev
   export PS2SDK=$PS2DEV/ps2sdk
   export PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH
   ```

   `make` also builds the host tools with the machine's own C++20 compiler, through **CMake 3.24 or
   newer**, so `cmake` has to be on `PATH` as well.

2. **PCSX2**, to run it on the emulator, plus a PS2 BIOS image for it. The Makefile's `run` target
   defaults to `/Applications/PCSX2.app/Contents/MacOS/PCSX2`; override `PCSX2=` elsewhere.

3. **The Quake game data.** It is not included here. Place it in an `id1/` directory at the root of
   the repository (it is `.gitignore`d): either the shareware `pak0.pak` (episode 1), or the
   registered game's `pak0.pak` and `pak1.pak`.

   ```
   quake-ps2/
     id1/
       pak0.pak
       pak1.pak   (registered game only)
       pak0.pkz   (optional, with pak1.pkz: compressed packs, see below)
       music/     (optional: the soundtrack, see below)
   ```

   **The shareware data** is freely available: the [Quake Official Archive][link_quake_archive] has
   id's shareware releases, and `pak0.pak` is in the v1.06 one. This port has been tested with that
   v1.06 shareware data and with the full game's data from a CD rip.

   **The soundtrack** is optional. Rip the Quake CD's audio tracks 2-11 to 16-bit WAVs named
   `id1/music/trackNN.wav` (NN = the CD track number), at 44.1 or 22.05 kHz, then run `make music`
   to encode them into the `trackNN.adp` files the game streams (the PS2's native ADPCM, about
   1.5 MB a minute). Only the `.adp` files need to go onto the console's drive. A track with no
   `.adp` plays from its `.wav`, at a higher cost.

   **Compressed packs** are optional as well, and worth making for a console. `make pkz` converts
   each `id1/pakN.pak` into a `pakN.pkz` beside it, a zip of the same files, most of them deflated:
   pak0 from 17.8 MB to 8.2 MB, pak1 from 32.7 MB to 14.9 MB. The game loads a `pakN.pkz` in place
   of the `pakN.pak` with its number, so a console's drive needs only the `.pkz` files, and a level
   load reads half the bytes off it ([Compressed packs](#compressed-packs)). In PCSX2, where reading
   costs next to nothing and inflating doesn't, the paks load faster (a release build went from
   e1m1 to e1m2 in 1.23 s from them, 1.50 s from the `.pkz`): keep the `.pkz` files out of the
   repository's `id1/` for emulator work, unless they are what you are testing.

### Building

Three dependencies are git submodules under [src/tools/](src/tools/):
[vclpp](https://github.com/glampert/vclpp) (the VU microprogram preprocessor),
[vu-checker](https://github.com/glampert/vu-checker) (checks on the VU toolchain's output) and
[miniz](https://github.com/richgel999/miniz) (deflate, for the save games and the compressed packs).
Clone with `--recursive`, or run `git submodule update --init --recursive` in an existing clone.

```sh
make            # debug build   -> build/debug/quake.elf (+ host tools)
make release    # optimized     -> build/release/quake.elf
make run        # build, then launch it in PCSX2
```

| | `make` (debug) | `make release` |
| --- | --- | --- |
| Optimization | `-O2` | `-O3` |
| Debug info | `-gdwarf-2 -gz` | none |
| Assertions | on | compiled out |
| Tests and other debug-only code | in | compiled out |

Both configs strip the ELF that runs and keep the symbols next to it in `quake_unstripped.elf`.
`make compiledb` regenerates `compile_commands.json` for clangd, and `make clean` removes all
build output.

[CI](.github/workflows/ps2.yml) runs `make` and `make release` on Linux for every push and pull
request that changes more than the documentation, inside the ps2dev project's
[`ps2sdk` container image](https://github.com/ps2dev/ps2sdk/pkgs/container/ps2sdk), pinned to
one build of it. It checks that both configs compile and link, and keeps no ELF: releases are
packaged from local builds, as described below.

The host tools in [src/tools/host/](src/tools/host/) (`unpak`, `imgdump`, `musenc`, `pakzip`, into
`build/tools/`) are a CMake project, [CMakeLists.txt](src/tools/host/CMakeLists.txt). `make`
configures it once into `build/tools/cmake/`, then runs its build every time, which does nothing
while the tools are up to date. They are built optimized and held to the game's strict warning set
as errors (`/W4` under MSVC). They build on Windows too, with MSVC, from the repository root:

```sh
cmake -S src/tools/host -B build/tools
cmake --build build/tools --config Release
```

That leaves the `.exe` files in `build/tools/Release/`, with the C/C++ runtime linked statically, so
they run without the Visual C++ Redistributable. A second [CI](.github/workflows/tools.yml) workflow
builds the tools alone on Linux (GCC), macOS (Apple clang) and Windows (MSVC) whenever they change,
and keeps the Windows executables as an artifact, `host-tools-windows-x64`, on each run's page under
the repository's Actions tab, for 90 days. Downloading it needs a GitHub sign-in.

[make_github_release.sh](src/tools/scripts/make_github_release.sh) `[-f] [-u] [version]` cuts a
release: a clean build of both configs, each config's `quake.elf` packaged with `LICENSE`,
[RELEASE_NOTES.txt](misc/RELEASE_NOTES.txt) (its `@VERSION@` and `@COMMIT@` filled in, so it points
to the exact source) and an `id1/` holding the default `config.cfg` and an empty `music/` folder,
zipped into `github_rel_<version>/`. `-u` adds `quake_unstripped.elf`. The player adds the paks.

### Running in PCSX2

`make run` symlinks `build/<config>/id1` to the repo's `id1/` and launches
`PCSX2 -batch -elf build/<config>/quake.elf`. PCSX2 needs a few settings that are **off by default**.
Edit `~/Library/Application Support/PCSX2/inis/PCSX2.ini` (macOS) **while PCSX2 is closed**, since it
rewrites the file on exit:

```ini
[EmuCore]
HostFs = true             ; host: maps to the ELF's directory; without it there is no game data

[Logging]
EnableIOPConsole = true   ; the game's stdout goes through the IOP
EnableFileLogging = true  ; ...and into logs/emulog.txt

[USB1]
Type = hidkbd             ; optional: a USB keyboard that passes host keystrokes through
```

The engine's console output is prefixed with `[Q1]` in the log:

```sh
tail -f ~/Library/Application\ Support/PCSX2/logs/emulog.txt | grep '\[Q1\]'
```

- **Keep the ELF's full path under 152 characters.** PCSX2 passes it to the BIOS, and a longer one
  crashes the BIOS's loader before the game starts. A release unzipped deep in a folder tree can
  get there.
- `make run RUN_ARGS="-heapsize 20480"` puts arguments on the game's command line. Like id's Quake,
  QuakeSpasm runs `+commands` from there only for the registered game; an `id1/autoexec.cfg` runs at
  boot either way, and is the way to script a session.

### Running on a console

Copy `quake.elf` (from a release, or `build/release/`) and `id1/` side by side into a folder on a
FAT32/exFAT USB drive or on a PFS partition of the internal HDD, and launch the ELF from a loader
such as wLaunchELF:

```
mass:/QUAKE/
  quake.elf
  id1/
    pak0.pkz   (or pak0.pak)
    pak1.pkz   (or pak1.pak; registered game only)
    music/     (optional: the trackNN.adp files)
```

- **Finding the game data.** The game looks in the folder it was launched from first, then at the
  drive roots: over USB only when launched from USB, and on the HDD and then USB otherwise. Every
  driver it needs is built into the ELF. If it finds nothing, the error screen lists where it
  looked.
- **The IOP is reset at every console boot**, and every driver loaded afresh, whatever the
  launcher left running: the reset costs ~175 ms, and a USB drive then takes ~0.6 s to come back
  up. From 2026-10-12 to 2026-11-01 the boot reused the launcher's drivers instead, which saved that
  time but, on the Quake II port's console, froze the game after its first level load whenever it
  went through an old launcher's USB driver (wLaunchELF 4.43a's usbhdfsd);
  `-DPS2_QUAKE_ALWAYS_RESET_IOP=0` brings that boot back, for comparisons. The `IOP boot:` lines in
  the console and in `quake.log` say what the launcher left running, what the boot loaded, and when
  each step finished.
- **Load times.** The HDD loads a level about twice as fast as the best USB setup. The PS2's USB is 1.1, so a
  faster stick doesn't help, but fewer bytes and larger clusters do. Copy the `.pkz` packs rather
  than the `.pak` ones: a load reads half the bytes (the Quake II port's loads over USB took 25-40%
  less time from its `.pkz`, and about as long from the HDD). And format the stick FAT32 with
  **32 KB clusters or larger** (macOS picks 8 KB for a 16 GB stick; `sudo newfs_msdos -F 32 -c 64
  -v NAME /dev/rdiskNsM` sets 32 KB, erasing the volume).
- **Saves and settings** go to the memory card in slot 1, never to the USB drive or the HDD.
- **NTSC or PAL.** The game follows the console's own standard. Options → Video Options → Video
  mode forces one, after a restart, so a PAL console can run at 60 Hz on a TV that takes it.
- **A log on the drive.** A console has no stdout to read: `ps2_logfile 1` (in `id1/autoexec.cfg`,
  or at the console) writes `quake.log` next to `id1/`.
- **The debug ELF** runs the same way, with assertions on and the debug overlays showing.

**Trying the console's storage path in PCSX2.** With `HostFs = false` the game boots as it would
on a console. [make_usb_image.py](src/tools/scripts/make_usb_image.py) (macOS) builds a FAT32 disk
image, `build/quake-usb.img`, holding both ELFs and a copy of `id1/`, to attach as a USB drive:

```ini
[USB2]
Type = Msd
Msd_subtype = 0
Msd_ImagePathMsd = /absolute/path/to/quake-usb.img
```

`[DEV9/Hdd]` `HddEnable` and `HddFile` attach an HDD image the same way. PCSX2 doesn't model
either drive's speed, so its load times say nothing about a console's.

### Debugging

| Command | What it does |
| --- | --- |
| `ps2_memstats` | Prints memory use by subsystem, and the heap's totals. |
| `ps2_vramstats` | Prints the GS texture heap block by block, with its totals and its peak this run. |
| `hunk_print` | QuakeSpasm's own: prints the hunk by block, with what remains. |
| `ps2_dump_iop_mods` | Prints the boot report, the IOP modules loaded and the IOP memory free. |
| `screenshot` | Writes the last frame to `id1/spasmNNNN.tga`. |
| `ps2_saveinfo` | Says where saves are going, and lists the memory card's saves. |
| `menu_debug` | Opens the debug page. |

**The debug page** (Options → Debug Options) puts the debugging aids on the gamepad, for a console
with no keyboard: the Quake console itself (no pad button opens it; Start closes it in a game), the
log file, the IOP report, the save list, a dump of the console's text, a screenshot, god mode and
all the weapons (`impulse 9`) in a single player game, and in the builds that have them, the tests
below. While the console is down, L2 and R2 scroll it.

**The log file.** `ps2_logfile 1` copies everything the console prints to `quake.log`, with a
timestamp per line, starting from the boot report. It also catches the reports printed on the way
to a fatal error (crash reports, stack traces, the render pipeline dump), so a crash on a console
still leaves a record; those are written even with the cvar off, behind the 16 KB of output the
log keeps while it is off, so a crash leaves a `quake.log` either way. Each line is a write to the drive, slow over USB, so leave it off when
timing anything.

**Render pipeline hangs.** In debug builds a wait on the GS that runs past a second dumps the state
of the whole graphics pipeline (DMA, VIF1, VU1, GIF, GS, the last DMA packets and VU1's memory) to
stdout and the log, then stops with an error screen.

**Tests and traces**, armed by cvars and build flags (see [CVARS.md](CVARS.md)):

- `ps2_testmaps 1` (debug builds) loads every level in turn and reports what each costs in RAM and
  in the GS texture heap.
- `ps2_testaio 1` (debug builds) reads a list of files through the background loader and through
  the engine's own loader and compares them, as the drive is and throttled to a USB stick's rate.
- `ps2_perftest 1` (profile builds) plays the three demos with a per-frame timing log, and
  `ps2_testmaps 2` turns a full circle at a few points on every level. The scripts in
  [src/tools/scripts/frame_log/](src/tools/scripts/frame_log/) summarize and compare the captures.
- `PS2_QUAKE_LOAD_TRACE` ([load_trace.h](src/ps2/debug/load_trace.h)) traces each step of a level
  load into the log, and `-DPS2_QUAKE_IO_LOG=1` ([io_log.h](src/ps2/debug/io_log.h)) records what each
  load read and how long it took, and from a `.pkz` what came off the drive and how long it spent
  inflating: the way to compare a `.pak` load with a `.pkz` one on a console.

---

## Rendering

QuakeSpasm draws through the public functions of its OpenGL renderer; the backend implements
those directly on the PS2's hardware, with no GL underneath.

- **The frame** is one DMA chain, built by the EE while the GS draws the previous one. 3D goes
  through VU1 microprograms; 2D goes straight to the GS. The wait for vsync that paces the frames
  sleeps on the vblank interrupt (a semaphore signalled from an `INTC_VBLANK_S` handler), so the
  8-11 ms a frame leaves before its field ends go to the other threads, the background loader
  above all; `ps2_vsync_spin 1` brings the old status-register spin back.
- **Textures** stay 8-bit in VRAM, as in the game's data, and read Quake's palette through a CLUT.
- **The world** ([view.cpp](src/ps2/renderer/view.cpp)) keeps QuakeSpasm's visibility code and draws
  in its passes: textures, then the lightmaps, then the fullbright texels. Water, slime and lava
  warp on VU1, and the sky ([sky.cpp](src/ps2/renderer/sky.cpp)) scrolls its two layers.
  Dynamic lights (muzzle flashes, rockets, explosions) rebuild the lightmaps they reach, as in
  QuakeSpasm, or with `ps2_dynamic_lights vertex` VU1 adds the four nearest the camera at each
  vertex of the lightmap pass, which costs the EE nothing but washes the walls a flat grey rather
  than brightening their texture. `gl_flashblend` draws QuakeSpasm's glows in their place.
- **Models** ([alias.cpp](src/ps2/renderer/alias.cpp)) are interpolated between poses on VU1 and lit
  as QuakeSpasm lights them, with their shadows (`r_shadows`) and the players' colours. QuakeSpasm
  keeps them in its cache, where room for something else can evict one, and reads it back the
  moment it is drawn; here that read goes to the background loader
  ([model_load.h](src/ps2/renderer/model_load.h)) and the entity sits out the few frames until the
  model is back, instead of the frame waiting on the drive.
  [Sprites](src/ps2/renderer/sprite.cpp) face the camera in all of QuakeSpasm's orientations, and
  [particles](src/ps2/renderer/particles.cpp) are QuakeSpasm's round dots, or with `r_particles 2`
  its squares.
- **2D** ([draw.cpp](src/ps2/renderer/draw.cpp)): the console, menus and status bar. Quake laid
  them out for 320x200, and at QuakeSpasm's size of 1 they come out small on the 640x448 screen,
  so here they are drawn at 1.5 times their size by default: `scr_menuscale`, `scr_conscale` and
  `scr_sbarscale` are 1.5 (QuakeSpasm's 1), which Options' Scale slider sets together. The status
  bar is half see-through (`scr_sbaralpha` 0.5, QuakeSpasm's 0.75). The main menu leaves out
  Multiplayer, which the PS2 doesn't play, and has an About page after Help/Ordering
  ([about_menu.cpp](src/ps2/menu/about_menu.cpp)): what the port is, its version, its author and
  its GitHub page. No game data has an "About" in the menu's lettering, so the ELF carries one,
  made from the menu pictures' own letters by
  [make_menu_about.py](src/tools/scripts/make_menu_about.py).
- **Video Options** (in the Options menu): NTSC or PAL, a 16- or 32-bit framebuffer (16-bit leaves
  more VRAM for textures), the picture's place on the TV (Screen X and Y, for a set that cuts an
  edge off: a white line marks the picture's edge while either row is under the cursor), the
  texture filtering, dithering, the dynamic lights (lightmaps, vertex or glows), the models'
  shadows, round or square particles, the GS latency, and the FPS counter and debug overlays.
  The first two apply after a restart. The Options page itself has the save device: the memory
  card, or the drive the game booted from (see [Save games](#save-games)).
- **Brightness and Contrast** (the Options page's sliders, QuakeSpasm's `gamma` and `contrast`):
  QuakeSpasm applies them to the finished frame, which the GS has no stage for, so here they go
  into the palette as software Quake's gamma did ([gamma.cpp](src/ps2/renderer/gamma.cpp)): into
  the CLUTs every texture samples through, and into the lightmaps' ramp, so a lit wall comes out as
  QuakeSpasm's shader would make it. The models' light takes the curve per model, their shading
  doesn't; the vertex dynamic lights, the glows and the view tint are left as they are. A 16-bit
  framebuffer bands more in darks a brightening lifts, which `ps2_fb_dither` softens.
- **The boot splash** shows the QUAKE plaque from the menus, built into the ELF, from the first
  moment of the boot until the game draws its first frame. **The loading screen** shows the loading
  plaque with a status line naming the file being read.

---

## Input

[input.cpp](src/ps2/input/input.cpp) drives the DualShock 2 ([pad.cpp](src/ps2/input/pad.cpp), held in
analog mode) and an optional USB keyboard ([keyboard.cpp](src/ps2/input/keyboard.cpp)).

**The pad** works as QuakeSpasm's game controller: each button is a key the menus understand (Cross
is Enter, Circle is Back) and binds like any other (`bind RTRIGGER +attack`). The defaults:

| Button | Key | Default bind |
| --- | --- | --- |
| Cross / Circle | `ABUTTON` / `BBUTTON` | `+jump` / `+movedown` (swim down) |
| Square / Triangle | `XBUTTON` / `YBUTTON` | |
| L1 / R1 | `LSHOULDER` / `RSHOULDER` | `impulse 12` / `impulse 10` (previous / next weapon) |
| L2 / R2 | `LTRIGGER` / `RTRIGGER` | `+jump` / `+attack` |
| L3 / R3 | `LTHUMB` / `RTHUMB` | |
| D-pad | the arrow keys | `+forward`, `+back`, `+left`, `+right` |
| Start / Select | `ESCAPE` / `TAB` | the menu / `+showscores` |

The left stick moves and strafes and the right one turns and looks, through QuakeSpasm's `joy_*`
cvars. The dead zones are wider than QuakeSpasm's, since a worn DualShock 2's stick can rest far
enough off centre to turn the camera on its own. Outside a game the left stick navigates the menus.

**The menus speak the pad's language.** Options' Controls page lists the binds by the pad's buttons
(Cross, Circle, Square, Triangle, L1 to R3, the D-pad's Up, Down, Left and Right, Select): Cross
changes one, Square clears it. A keyboard's binds stay in `config.cfg`, but the page leaves them out.
The Options rows QuakeSpasm gives the mouse work the look stick instead:

- **Look Speed** scales `joy_sensitivity_yaw` and `joy_sensitivity_pitch` together, 0.5 to 2; at 1
  they are QuakeSpasm's 240 and 130 degrees a second.
- **Invert Look** is `joy_invert`.
- **Stick Look** is `+mlook`, mouse look for the stick. Off, the look stick only turns, as Quake's
  mouse does without mouse look, and the view levels itself as in classic Quake. It is on by
  default, and `config.cfg` keeps a `-mlook` when it is turned off.
- **Lookspring** recentres the view as the player runs after looking up or down.

Lookstrafe is gone: it had the mouse strafe instead of turn, and a look stick that strafed would
leave nothing to turn with. The yes/no questions ("start a new game?") take Cross for yes and
Circle for no.

**Rumble** ([rumble.cpp](src/ps2/input/rumble.cpp)) gives short bursts for the player's shots, damage
taken (stronger the harder the hit), pickups and powerups. It is off in menus, demos and while
paused; the Options page's Rumble row (`in_rumble`) turns it off.

**A USB keyboard** is a development tool here, so it is off by default in release builds
(`in_keyboard 1` turns it on). It types at the console, with the keys mapped by position as on a US
layout.

---

## Sound

QuakeSpasm's own mixer runs unchanged, at 22050 Hz, and the backend ([audio/](src/ps2/audio/))
streams what it mixes to the SPU2 through the audsrv IOP driver. A feeder thread keeps that stream
topped up, so the sound carries on cleanly through stalls such as a level load or a save. The
audsrv is the game's own copy of ps2sdk's ([ps2/iop/audsrv](src/ps2/iop/audsrv/README.md)), started
with its RPC thread one priority above fileXio's: the SDK's answered at fileXio's priority, and the
IOP has no time slicing, so on the HDD the feeder's calls waited out any file read in progress, up
to 65 ms of audsrv's 106 ms ring on the Quake II port's console, which is the audible stutter at a
pickup there. The rest of the IOP modules are the SDK's.

**Sounds first asked for mid-level load in the background** ([sound_load.h](src/ps2/audio/sound_load.h)):
`S_LoadSound` outside a level's precache (and with the loading plaque down) hands the WAV to the
background loader (see [Compressed packs](#compressed-packs)), and the pump that runs before each
frame resamples it into the cache once it lands, so the frame that first hears a menu click, the
talk beep or a sound the cache had evicted no longer reads the file (58 ms from the HDD, 147 ms from
a USB stick on the Quake II port's console). A sound asked for while still loading starts when it
lands, if that is within half a second, and is dropped otherwise. Level loads stay synchronous.

**CD music.** The PS2 can't play the soundtrack off the Quake CD, so it plays it from files instead:
`id1/music/trackNN.adp`, made from a CD rip by `make music` (see [Prerequisites](#prerequisites)),
or the `.wav` files directly. It behaves as QuakeSpasm's external music does: "Background Music"
(QuakeSpasm's "External Music", `bgm_extmusic`) and "Music Volume" in the options menu, `music_loop`,
and the `music`, `music_stop`, `music_pause` and `music_resume` commands. The format is the Quake II port's, so its tracks play here too.
[`MusicStream`](src/ps2/audio/music_stream.h) keeps two read buffers in flight, 32 KB each for
ADPCM (~1.3 s), up to 64 KB each for a WAV, and decodes on demand; the buffers come from the heap
when a track starts and go back when it stops. The file reads go to the background loader (see
[Compressed packs](#compressed-packs)) at its stream priority, since a blocking read off a USB stick
could otherwise cost a frame; one it declines is read on the main thread when it is needed.

---

## Compressed packs

QuakeSpasm's file system is kept as it is: its search path, its `pakN.pak` archives and its
lookups ([common.c](src/quake/common.c)). For each `pakN.pak` it now looks for a `pakN.pkz` first,
and loads that instead. A pkz is a standard zip ([pkz_format.h](src/ps2/fs/pkz_format.h)), written
by `pakzip` (`make pkz`) with the pak's names in the pak's order, so any zip tool reads it
(`unzip -t id1/pak0.pkz`), and the game loads one made by another tool as well. Each file is
deflated, or stored if that saves less than 10% (a few sounds), since inflating costs the console
time: the maps shrink to 45% of their size, the models to 39-47%, the 2D art to a quarter, the
sounds to 71-87%. The format and the tool are the Quake II port's.

[pkz_pack.cpp](src/ps2/fs/pkz_pack.cpp) inflates on the EE, with the miniz already linked for the
save games. A file read whole, as every map, model, sound and texture is, inflates straight into
the memory it is loaded to. A stream of a deflated file inflates through a 32 KB window as it is
read, and can seek: `pakzip` leaves a restart point every 32 KB, for 0.1% of the pack's size. Both
packs' tables and the whole-file workspace take 58 KB (tag `Pack`), and an open stream of a
deflated file 72 KB more. Debug builds also check the CRC of every file they read whole. The demos
are stored, not deflated (`pakzip --store-ext .dem`, what `make pkz` passes), so they can stream
through the background loader below; a pkz made before 2026-11-01 has them deflated, and they then
read on the main thread as before.

**Background loading** ([async_io.h](src/ps2/fs/async_io.h), `ps2::aio`). Every file read sleeps
its caller until the IOP's file server answers, 41 ms for a 32 KB chunk on a USB stick, so a read
made mid-level by the main thread is a dropped frame. `COM_LocateFile` resolves a file to a
descriptor and a byte range without reading it (a pak's second descriptor, a pkz entry's
compressed range, a loose file's own stream), and a thread one priority below the main one reads
it in 32 KB chunks aligned to the file, or inflates a deflated entry straight into the caller's
buffer, re-picking the best (priority, age) request after every chunk. It runs only while the
main thread sleeps, which since the vsync wait sleeps on the vblank interrupt is most of every
frame. Destinations are 64-byte aligned SIF DMA targets; the loader never allocates, prints or
touches the engine, and the places that free or close behind a read drain it first. The design
and its console measurements are the Quake II port's (`misc/docs/plans/background-loading.md`
there); `ps2_testaio 1` is its self-test, and `ps2_aio 0` turns it off. Tag `Aio`.

What goes through it: demos, the sounds first asked for while a level runs (see
[Sound](#sound)), and the alias models the cache evicted and a draw wants back (see
[Rendering](#rendering)). Level loads stay synchronous under the plaque, where a stall is expected,
and so do the menu pictures, which load as a menu opens. An asset that is still loading is skipped
where it would be drawn or played, never shown as a placeholder; a missing one behaves as before,
since the existence check stays synchronous. QuakeSpasm has no other mid-level file read: its
textures come out of the level and model files, so none is loaded from a file of its own.

**Demos stream through it** ([readahead_stream.h](src/ps2/fs/readahead_stream.h)). `COM_FOpenFile`
on a `.dem` whose source can be read by range (a pak, a stored pkz entry, a loose file) returns an
unbuffered `fopencookie` stream over a ring of two 32 KB slots, each a loader request from the
open, so cl_demo.c's `fscanf`, `fread` and `fclose` work unchanged and a frame's reads copy from
memory. Before, the stream refilled on the main thread: a deflated pkz entry inflated through its
window as it was read, and on the Quake II port's console each 64 KB refill of a demo froze a
frame for 80 ms from the HDD. `ps2_readahead_kb 0` gives every demo the old stream.

---

## Save games

Games are saved to the memory card in slot 1 by default ([save/](src/ps2/save/)), in a `Q1PS2` folder
the PS2's browser shows with a 3D quad damage icon. The Options page's Save Device row
(`ps2_savedevice`) can keep them on the drive the game booted from instead: the USB drive (`usb`),
the hard disk (`hdd`), or under PCSX2 the host filesystem (`host`), in `id1/save/` there, as the
Quake II port keeps them in `baseq2/save/`. Every device holds the same files: each of the menus'
20 slots is one compressed file (an e1m1 save takes 11 KB), and a save is written in full before
the one it replaces is deleted, so an interrupted save never loses the previous one. Loading reads
the selected device only.

Whenever the game reads or writes saved data (a save, a load, the save menus listing their slots,
the settings), it shows the notice PS2 games show ([save_notice.h](src/ps2/save/save_notice.h)):
"Data is being saved to the Memory Card in slot 1. Do not remove the Memory Card or reset/switch
off the console." ("loaded from" for a read), naming the device in use, in Quake's own message box.
On a console the game stands still for a second or more while the card is busy, so the notice goes
up before the device is touched, the music fades out under it rather than stopping mid-note, and
once the device is done the music fades back in from where it stopped.

When the save device can't be used (no memory card in the slot, say), the load and save menus say
so over their slots, with how to switch to the other device, and a save or a load that fails says
why in a message box, dismissed with Cross.

`config.cfg`, the binds and settings, lives with the saves. It is written on quit and when leaving
the options menu:

| | Game data on `host:` (PCSX2) | Game data on the HDD or USB (console) |
|---|---|---|
| **Saving** | `id1/config.cfg` always, plus the card's copy when saves go to the card | the save device's: the card's, or `id1/config.cfg` on the drive |
| **Loading** | `id1/config.cfg` first, the card's if there is none | the drive's `id1/config.cfg` if it says saves go to the drive; otherwise the card's first, then the drive's |

So a player who keeps everything on the USB drive or the hard disk never has the card touched at
boot. Switching the save device from the drive back to the card rewrites the drive's
`id1/config.cfg` once more, so that it no longer names the drive and the next boot reads the card.
The card's copy is read at boot under the notice, drawn over the QUAKE plaque since the renderer
hasn't started yet. An unchanged config isn't written again, so leaving the options menu without
changing anything touches no device.

The repository tracks `id1/config.cfg` as the default settings. The game writes it deterministically,
so a run that changes no setting writes it back byte for byte, and `git status` shows when one did.

---

## Source layout

```
src/
  quake/                QuakeSpasm's C: engine, client, server, QuakeC VM
  ps2/                  the PS2 backend - all new C++ code
    system/             main() entry point, Sys_* seam, IOP boot, dlmalloc heap
    renderer/           GS front-end, VRAM heap, textures, models, VU1 path
      vu1progs/         VU1 microprograms (.vcl)
    audio/              SNDDMA_* seam, audsrv device, mix ring, CD music streaming
    input/              IN_* seam, DualShock pad and rumble, USB keyboard
    math/               vector/matrix math for the renderer
    net/                the network driver table (loopback only)
    save/               save games on the memory card or the boot drive
    fs/                 compressed game packs (pakN.pkz) for QuakeSpasm's file system
    debug/              on-screen error printing, the log file, stack traces, EE exception handling
    menu/               the main menu's About page
    tests/              standalone bring-up scenes and tests
    builtin/            images compiled into the ELF (the QUAKE plaque, the main menu's About item, the palette), from imgdump
  tools/
    host/               host-side command line tools (unpak, imgdump, musenc, pakzip)
    scripts/            Python helpers (symbolize, compile_commands.json, frame logs, the USB image, the About item) and make_github_release.sh
    vclpp/              VCL preprocessor for the VU microprograms (git submodule)
    vu-checker/         openvcl/dvp-as output checks run by every VU build (git submodule)
    miniz/              deflate codec for the save games, the compressed packs and pakzip (git submodule)
    vscode_extensions/  VCL/VU assembly syntax highlighting for VSCode
```

---

## Credits

- [QuakeSpasm][link_quakespasm], which this port derives from: everything under [src/quake/](src/quake/)
  is QuakeSpasm 0.97.0's code, by the QuakeSpasm developers, changed only where tagged `[PS2_QUAKE]`.
  QuakeSpasm is itself based on John Fitzgibbons' FitzQuake.
- id Software's Quake, released under the GPL in 1999 ([source][link_id_repo]; the original release
  notes are in [src/README.md](src/README.md)).
- The PS2 backend under [src/ps2/](src/ps2/) comes from the [Quake II PS2 port][link_q2_ps2].

This project also includes or utilizes other Open Source libraries:
- [miniz](https://github.com/richgel999/miniz)
- [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html)
- [ps2sdk](https://github.com/ps2dev/ps2sdk)

---

## License

Quake was released by id Software under the GNU General Public License, and QuakeSpasm is
distributed under the same terms. New code written for the PS2 port and any changes made to the
original source code are also released under the GNU General Public License version 2. See the
accompanying LICENSE file for details.

[link_quakespasm]: https://github.com/sezero/quakespasm
[link_id_repo]: https://github.com/id-Software/Quake
[link_q2_ps2]: https://github.com/glampert/quake2-ps2
[link_ps2_dev]: https://github.com/ps2dev
[link_quake_archive]: https://github.com/Jason2Brownlee/QuakeOfficialArchive

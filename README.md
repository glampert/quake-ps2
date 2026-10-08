
# Quake port for the PlayStation 2

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
Quake II port: its renderer (GS front-end, VRAM texture heap, VU1 microprograms), input, audio, memory and
debugging code carry over, ported to QuakeSpasm's interfaces.

### Status

**Early bring-up.** QuakeSpasm boots and runs on the PS2: in PCSX2 it finds the game data on
`host:`, draws its console, menus and status bar, and takes the DualShock pad and a USB
keyboard, through the attract-mode demos and every shareware map. The 3D view draws all of it:
the world - textured, lightmapped, with its fullbright texels, warping water and scrolling sky -
and the brush models, monsters, items, view weapon, sprites and particles, and the sound effects
and the soundtrack play through the SPU2. Games save to the memory card. Its PC-sized limits are cut down to fit the PS2's 32 MB. The port is
brought up in phases, each checked in PCSX2:

1. Compile QuakeSpasm with the EE toolchain. *Done.*
2. Link and boot, rendering nothing and logging to stdout. *Done.*
3. Game data and the game loop, headless. *Done.*
4. 2D: console, menus, HUD. *Done.*
5. Input: DualShock and USB keyboard. *Done.*
6. 3D: world, lightmaps, water, sky, models, sprites, particles. *Done*, but for fog, which no
   shareware map uses.
7. Sound, CD music, save games. *Done.*

This section says what works as each phase lands.

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
       music/     (optional, trackNN.adp or trackNN.wav soundtrack - see below)
   ```

   The soundtrack is optional. Rip the Quake CD's audio tracks 2-11 to 16-bit PCM WAVs named
   `id1/music/trackNN.wav` (NN = the CD track number; any case), then run `make music` to encode
   them into the `trackNN.adp` files the game streams: SPU2 ADPCM at 22050 Hz, about 1.5 MB a
   minute. Only the `.adp` files need to go onto a USB stick. Input must be at 22050 Hz or exactly
   twice that (a CD rip); convert anything else first, e.g. `afconvert -f WAVE -d LEI16@22050 -c 2
   in.flac out.wav` on macOS. Skipping `make music` works too: a track with no `.adp` plays from its
   `trackNN.wav`, at seven times the bytes read and more EE time to resample a 44.1 kHz rip.

### Building

Three dependencies come in as git submodules: [vclpp](https://github.com/glampert/vclpp), the
preprocessor the VU microprograms go through, at [src/tools/vclpp/](src/tools/vclpp/) (with a submodule
of its own), [vu-checker](https://github.com/glampert/vu-checker), the checks every VU build runs over
the toolchain's output, at [src/tools/vu-checker/](src/tools/vu-checker/), and
[miniz](https://github.com/richgel999/miniz), the deflate codec for save games, at
[src/tools/miniz/](src/tools/miniz/). Clone with `--recursive`, or run
`git submodule update --init --recursive` in an existing clone.

```sh
make            # debug build   -> build/debug/quake.elf (+ host tools)
make release    # optimized     -> build/release/quake.elf
make run        # build, then launch it in PCSX2
```

| | `make` (debug) | `make release` |
| --- | --- | --- |
| Optimization | `-O2` | `-O3` |
| Debug info | `-gdwarf-2 -gz` | none |
| `PS2_Assert` / `PS2_AssertMsg` | on | compiled out |
| `PS2_QUAKE_DEBUG` (tests, debug-only code) | in | compiled out |

Both configs strip the ELF that runs; the symbols stay next to it in `quake_unstripped.elf`, which is
the one to feed `addr2line` or `build/tools/symbolize`. `make compiledb` regenerates
`compile_commands.json` for IntelliSense/clangd, and `make clean` removes all build output.

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

`make run RUN_ARGS="-heapsize 20480"` puts arguments on the game's command line (PCSX2's
`-gameargs`). `+commands` there do nothing with the shareware data: like id's Quake,
QuakeSpasm only runs them for the registered version. An `id1/autoexec.cfg` runs at boot
either way, as on the desktop, and is the way to script a session.

### Debugging commands

| Command | What it does |
| --- | --- |
| `ps2_memstats` | Prints the backend's memory tags, and dlmalloc's arena, in-use, untagged-malloc and free totals. |
| `hunk_print` | QuakeSpasm's own: prints the hunk by block, with what remains. |
| `ps2_dump_iop_mods` | Lists the IOP modules currently loaded. |
| `screenshot` | QuakeSpasm's own, TGA only: reads the last frame back out of GS VRAM and writes `id1/spasmNNNN.tga`. |

---

## Rendering

QuakeSpasm draws through the public functions of its OpenGL renderer, and the backend
implements those directly, with no GL underneath. All of it draws but fog (`gl_fog.c` still
parses it, so a map that sets it plays, unfogged).

- **Video and the frame** ([vid.cpp](src/ps2/renderer/vid.cpp)). `VID_Init` brings the GS up at
  640x448 with two framebuffers, 16-bit by default (`ps2_fb_16bit`), and a 16-bit z-buffer.
  `SCR_UpdateScreen` draws each frame between `GL_BeginRendering`, which clears it to
  `r_clearcolor`, and `GL_EndRendering`, which submits it. By default the GS draws a finished
  frame while the EE builds the next one (`ps2_gs_latency`).
- **Palette.** Quake's textures stay 8-bit in VRAM (PSMT8) and sample `gfx/palette.lmp`
  through a CLUT, built once as the GS comes up.
- **Command buffer.** A frame is one DMA chain to VIF1, built in a 1 MB block of two 512 KB
  halves, so one can fill while the other draws. 3D batches go to the VU1 microprograms, and 2D
  goes straight to the GS.
- **2D** ([draw.cpp](src/ps2/renderer/draw.cpp)), in place of QuakeSpasm's `gl_draw.c`: console
  text, pics, fills, the tiled border, and QuakeSpasm's canvases, each of whose
  `glOrtho`/`glViewport` pairs becomes a scale and an offset. WAD pics under 64x64 share two
  256x256 scrap atlases, as in QuakeSpasm; every other pic gets its own texture. The menus and
  the status bar default to twice their size (`scr_menuscale`, `scr_sbarscale` 2), which fills
  the 640x448 screen the way the original filled 320x200. At that size QuakeSpasm's slider
  values, which sit past the menu's right edge, are cut off.
- **The world** ([view.cpp](src/ps2/renderer/view.cpp)), in place of QuakeSpasm's
  `gl_rmain.c`, `r_world.c` and `r_brush.c`: its visibility code as it is (`R_MarkSurfaces`
  walking the PVS onto the textures' chains), and its multipass drawing on VU1 - each chain's
  textures, then the lightmaps multiplying the framebuffer by the light (up to nearly twice,
  QuakeSpasm's `gl_overbright`), then the fullbright texels added over that. Water, slime and
  lava ripple on VU1 with QuakeSpasm's warp, an 8-texel sine every 128 texels, bent at the
  corners of a 32-unit grid their surfaces are cut on (so `gl_subdivide_size` does nothing).
  Brush entities run the same passes
  under their own transform. With `gl_flashblend` the dynamic lights glow as additive fans
  instead of lighting the walls.
- **Sky** ([sky.cpp](src/ps2/renderer/sky.cpp)), in place of `gl_sky.c`: QuakeSpasm's two
  scrolling layers on a box around the camera, tessellated by `r_sky_quality` (12, at most 16
  here) and drawn only over what the sky surfaces in view cover; `r_fastsky` draws the flat
  colour. The GS's z-test can't pass "farther", which QuakeSpasm's drawing order needs, so the
  order turns round: the layers go down first, then the sky surfaces' depth alone, then the
  world. Skyboxes (`sky`, worldspawn's `sky`) need external images, which the PS2 doesn't load.
- **Alias models** ([alias.cpp](src/ps2/renderer/alias.cpp)), in place of `r_alias.c` and
  `gl_mesh.c`: the monsters, items and view weapon, lerped between two poses on VU1. The EE
  copies each corner's two pose words into the frame as they are, and VU1 converts, lerps and
  transforms them. What else a corner needs - which pose vertex it is, its skin coordinates - is
  built once, when the model first loads, and read where it lies. They are lit as QuakeSpasm
  lights them (`gl_overbright_models` included), with the skin's fullbright texels drawn over
  again from the same vertices. The view weapon's depth is squeezed into the near end of the
  z-buffer. `r_shadows` draws QuakeSpasm's flattened shadows. Players wear their colours.
- **Sprites and particles** ([sprite.cpp](src/ps2/renderer/sprite.cpp),
  [particles.cpp](src/ps2/renderer/particles.cpp)): sprites as quads in all five of
  QuakeSpasm's orientations, unlit, their holes cut by the alpha test; particles as QuakeSpasm's
  discs (or squares, `r_particles 2`), each a GS sprite VU1 expands, growing with distance.
- **Draw data** ([brush.cpp](src/ps2/renderer/brush.cpp)): every brush surface's vertices,
  baked as the VU1 path takes them when a map loads, and the lightmaps
  ([lightmap.cpp](src/ps2/renderer/lightmap.cpp)): 256x256 atlases of one byte per luxel,
  rebuilt when a light style moves or a dynamic light reaches them.
- **Textures** ([texmgr.cpp](src/ps2/renderer/texmgr.cpp)): the BSP's textures stay on the
  hunk, all four of id's mip levels, and upload from there. Skins and sprite frames are copied,
  their rows padded to 16 texels, since the model file they come from is gone once it has
  loaded; a skin's glow texture draws from the skin's copy. Each samples through the CLUT for
  the palette QuakeSpasm would pick: whole, with the fullbright range black for the lit pass,
  or that range alone for the glow pass. A texture the engine frees while a frame is being
  recorded - a model the cache evicts mid-frame - is released once that frame has drawn.
- **Debug overlays** ([overlays.cpp](src/ps2/renderer/overlays.cpp)): an FPS counter and
  panels for frame times, memory, VRAM and draw statistics, on by default in debug builds
  (see [CVARS.md](CVARS.md)).
- **Screenshots.** `screenshot` reads the last finished frame back out of VRAM, turning the GS
  bus around for it, and writes `id1/spasmNNNN.tga`.

---

## Input

[input.cpp](src/ps2/input/input.cpp) implements QuakeSpasm's `IN_*` seam, in place of its SDL
`in_sdl.c`, over the DualShock pad ([pad.cpp](src/ps2/input/pad.cpp): libpad on the ROM's PADMAN,
locked into analog mode) and a USB keyboard ([keyboard.cpp](src/ps2/input/keyboard.cpp): the
ps2kbd driver, read raw).

**The pad** follows QuakeSpasm's game controller model. Each button sends one of the controller
keys QuakeSpasm already has, so the menus take it (Cross is Enter, Circle is Back) and it binds
like any other key (`bind RTRIGGER +attack`). id's `default.cfg` binds none of them, so the PS2
adds the binds below right after it runs (`PS2_DefaultConfig`, called from `cmd.c`), which puts
them back on the options menu's "Reset to defaults" too. Most are QuakeSpasm's own pad binds.

| Button | Key | Default bind |
| --- | --- | --- |
| Cross / Circle | `ABUTTON` / `BBUTTON` | `+jump` / `+movedown` (swim down) |
| Square / Triangle | `XBUTTON` / `YBUTTON` | |
| L1 / R1 | `LSHOULDER` / `RSHOULDER` | `impulse 12` / `impulse 10` (previous / next weapon) |
| L2 / R2 | `LTRIGGER` / `RTRIGGER` | `+jump` / `+attack` |
| L3 / R3 | `LTHUMB` / `RTHUMB` | |
| D-pad | the arrow keys | id's `+forward`, `+back`, `+left`, `+right` |
| Start / Select | `ESCAPE` / `TAB` | the menu / id's `+showscores` |

The left stick moves and strafes and the right one turns and looks, through QuakeSpasm's `joy_*`
cvars (dead zones, response exponents, sensitivities, `joy_invert`, `joy_swapmovelook`) at its
defaults. The defaults also turn `cl_alwaysrun` on, so the stick's deflection sets the pace all
the way up to a run. Outside a game the left stick works as the arrow keys, and held buttons
repeat, so menus scroll. Unlike QuakeSpasm's, `joy_enable 0` switches off only the sticks: the
pad is all the input most PS2s have.

**Rumble** ([rumble.cpp](src/ps2/input/rumble.cpp)) answers what happens to the player with short
bursts on the pad's two motors, which overlap when they run at the same time: each weapon's shots
(by the muzzle flash the server flags on the player), damage taken (from `V_ParseDamage`, stronger
the harder the hit), pickups (the bonus flash every pickup sends) and powerups coming on. It is
off in menus, the console, demos and while paused; `in_rumble 0` turns it off.

**A USB keyboard** is optional (`in_keyboard`). It sends keys by position, as on a US layout, and
types through QuakeSpasm's text input (`Char_Event`) with Shift applied; the last key pressed
repeats while held. `in_debugkeys 1` prints every pad button and keyboard usage as it arrives,
mapped or not. `in_keyboardmap <usage> <key>` points a USB usage at another key: PCSX2's
passthrough keyboard sends usage 0x34, the apostrophe, for the host's `` ` ``, so 0x34 opens the
console by default, and `in_keyboardmap 0x34 '` gives the apostrophe back on real hardware.

---

## Sound

QuakeSpasm's mixer (`snd_dma.c`, `snd_mix.c`, `snd_mem.c`) runs unchanged. The backend
([audio/](src/ps2/audio/)) implements its `SNDDMA_*` seam in place of `snd_sdl.c`:

- [`AudsrvDevice`](src/ps2/audio/audsrv_device.h) brings up `libsd.irx` and `audsrv.irx` (both
  embedded in the ELF) and opens a 16-bit stereo stream. The SPU2 is only reachable from the IOP,
  so every mixed byte crosses SIF through audsrv's RPCs, and audsrv upsamples it to the SPU2's
  48 kHz.
- [`MixRing`](src/ps2/audio/mix_ring.h) is the 64 KB buffer the mixer paints into. The play
  position reported back to the engine is how far the backend has submitted, not where the SPU2
  is, as WinQuake's waveOut backend did: it advances at the playback rate and never goes back,
  which is what QuakeSpasm's `GetSoundtime` needs.
- **The feeder** ([snd.cpp](src/ps2/audio/snd.cpp)) plays the part a sound card's DMA plays on a
  PC. It is a thread one priority level above the main thread, woken every 5 ms by a timer
  alarm, that keeps audsrv's queue 43 ms deep from what the mixer has painted. audsrv's IOP
  thread never checks for an empty queue: fed once a frame, any stall longer than its 106 ms
  queue (a level load, a save) would replay the last 106 ms in a loop, a loud buzz. The feeder
  carries on through a stall, padding the queue with silence once the painted audio runs out,
  without moving the position the engine reads back, so the mix picks up where it stopped. The
  IOP round trips happen on its thread too, off the main thread's frame.

**The mixing rate** is 22050 Hz: QuakeSpasm's `snd_mixspeed`, which defaults to 22050 on the PS2
instead of 44100. Quake's sound effects are 11025 Hz, and each is kept resampled to the mixing
rate in the hunk's cache, so 44100 would quadruple that (3.3 MB at 22050 in demo1) and the
mixing, and bring in QuakeSpasm's lowpass filter on top. 22050 is also the CD music's rate.
`-mixspeed <rate>` on the command line picks another rate audsrv takes: 11025, 12000, 22050,
24000, 32000, 44100 or 48000. Sound starts before `config.cfg` runs, so the cvar itself can't
change it.

In PCSX2 (debug build, demo1) the mixing costs about 0.42 ms of the main thread's frame. The
feeder is awake about 0.45 ms a frame, most of it waiting on the IOP while the main thread
runs. A new sound reaches the speakers after `_snd_mixahead` (0.1 s) plus the 43 ms queue.
`-nosound` turns sound off, as on QuakeSpasm, and an IOP driver that doesn't come up only costs
the sound.

**CD music.** QuakeSpasm plays a map's track off the Quake CD, or else from a loose
`music/trackNN` file through its codec libraries. This port reads no audio CD and has no codec
layer, so [cd_audio.cpp](src/ps2/audio/cd_audio.cpp) implements QuakeSpasm's `BGM_*` music over
loose `id1/music/trackNN.adp` files, SPU2 ADPCM: the PS2's native sample format, 3.5 times
smaller than 16-bit PCM, encoded by `make music` ([musenc](src/tools/host/musenc.cpp)). A track
with no `.adp` plays from its `trackNN.wav`. [`MusicStream`](src/ps2/audio/music_stream.h) keeps
two read buffers in flight (8 KB each for ADPCM, up to 64 KB each for a WAV), allocated only
while a track plays, and decodes on demand; the file reads come from a reader thread at the
main thread's priority. The music goes into the mixer's raw-sample channel, as `bgmusic.c`'s
did, so it reaches the SPU2 inside the audsrv stream; a 44.1 kHz WAV goes through a 23-tap
half-band decimator ([half_band.h](src/ps2/audio/half_band.h)) on the way in, and other rates
through a linear resampler. The file format is the Quake II port's, so its tracks play here too.

It behaves as `bgmusic.c` does: `bgm_extmusic` (the options menu's "External Music") turns it on
and off, `bgmvolume` ("CD Music Volume") sets the level, `music_loop` decides whether a track
starts over when it ends, and `music <name>`, `music_stop`, `music_pause` and `music_resume`
drive it from the console. Turning `bgm_extmusic` off stops the music and turning it on starts
the map's track again, right away rather than at the next map. In PCSX2 the ADPCM decode costs
about 0.16 ms of EE time per frame, and mixing the music in another 0.08 ms.

---

## Save games

QuakeSpasm saves a game as one text file, `<gamedir>/<name>.sav`, written through stdio, and its
load and save menus read each slot's comment line back the same way. On the PS2 those three
spots go through the backend ([save/](src/ps2/save/)) instead, and the saves live on the memory
card in MEMORY CARD slot 1:

- **A slot** (`s0` to `s19` from the menus, any name from the console's `save`) is one archive
  file on the card: a header carrying the menu's comment line, then the `.sav` text deflated
  ([slot_archive.cpp](src/ps2/save/slot_archive.cpp)), CRC-checked throughout. A save is written
  to the slot's *other* file (`<slot>_a.q1s` / `<slot>_b.q1s`) before the previous copy is
  deleted, so a failed or interrupted save never loses the one before it. The text is deflated
  as `Host_Savegame_f` writes it ([packed_blob.cpp](src/ps2/save/packed_blob.cpp)): an 83 KB
  e1m1 save takes 11 KB of the card, and about 0.7 s to write.
- **The card** is reached through libmc and the ROM's MCMAN/MCSERV
  ([memcard.cpp](src/ps2/save/memcard.cpp)), in a `Q1PS2` directory with the `icon.sys` and 3D
  icon the PS2 browser shows: the quad damage, built from the game's own `progs/quaddama.mdl`
  ([mc_icon.cpp](src/ps2/save/mc_icon.cpp)).
- **Running from `host:`**, the archived `ps2_savedevice` cvar keeps saves as QuakeSpasm does,
  plain `.sav` files in `id1/` (`host`, the default there), or sends them to the card (`mc`).
  `ps2_saveinfo` says where they go and lists the card's files.

`config.cfg` - the binds and archived cvars - is kept where each platform keeps settings
([save_api.cpp](src/ps2/save/save_api.cpp)):

| | Game data on `host:` | Game data on HDD (`pfs0:`) or USB (`massN:`) |
|---|---|---|
| **Saving** | `id1/config.cfg` on the host, as always; plus the card's `Q1PS2/config.cfg` when saves go to the card (`ps2_savedevice mc`) | the card's `Q1PS2/config.cfg` only. Never the game-data drive |
| **Loading** | the host's `id1/config.cfg` first - the file to edit by hand while developing; the card's if there is none | the card's first - the player's own settings; the drive's `id1/config.cfg` only if the card has none |

The cvars read ahead of `config.cfg` running (`ps2_fb_16bit`, `in_keyboard`) are read from the same
file. QuakeSpasm writes the config on quit, which a console rarely sees, so leaving the options
menu writes it too; the card copy is only rewritten when it changed. A card that is missing or
full just skips it, with a line on the console.

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
    save/               save games on the memory card
    debug/              on-screen error printing, stack traces, EE exception handling
    tests/              standalone bring-up scenes and tests
  tools/
    host/               host-side command line tools (unpak, musenc)
    scripts/            Python helpers (symbolize, compile_commands.json, frame logs)
    vclpp/              VCL preprocessor for the VU microprograms (git submodule)
    vu-checker/         openvcl/dvp-as output checks run by every VU build (git submodule)
    miniz/              deflate codec for the save games (git submodule)
    vscode_extensions/  VCL/VU assembly syntax highlighting for VSCode
```

Much of `src/ps2` is still the Quake II port's code, waiting for the phase that ports it.

---

## Credits

- [QuakeSpasm][link_quakespasm], which this port derives from: everything under [src/quake/](src/quake/)
  is QuakeSpasm 0.97.0's code, by the QuakeSpasm developers, changed only where tagged `[PS2_QUAKE]`.
  QuakeSpasm is itself based on John Fitzgibbons' FitzQuake.
- id Software's Quake, released under the GPL in 1999 ([source][link_id_repo]; the original release
  notes are in [src/README.md](src/README.md)).
- The PS2 backend under [src/ps2/](src/ps2/) comes from the [Quake II port][link_q2_ps2].

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

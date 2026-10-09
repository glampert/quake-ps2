# PS2 backend cvars

Every cvar registered by the PS2 backend under [src/ps2/](src/ps2/), with its default in the
debug (`make`) and release (`make release`) builds. See the [README](README.md) for the
bigger picture. QuakeSpasm's own cvars are not listed here, including the ones the backend
registers in place of the QuakeSpasm files it replaced (`gamma`, `r_lerpmodels`, the `joy_*`
cvars, ...). Where one of those means something different on the PS2 (`joy_enable`,
`in_debugkeys`), the README's section on that subsystem says so.

- **Arch.** marks `CVAR_ARCHIVE` cvars: they are written to `config.cfg` and read back on
  the next boot, so the value in your config, not the default, is what a run starts with.
- **—** in the release column means the cvar is not registered at all there: the code that
  reads it is compiled out with `PS2_QUAKE_DEBUG` / `PS2_QUAKE_PROFILE`. Archived cvars never
  get a **—**. Every build registers them, even where nothing reads them, because the quit
  drops an archived cvar the build never registered from `config.cfg`.

More come back as each subsystem is ported.

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_gs_latency` | 1 | 1 | Arch. | 1 leaves each frame drawing on the GS and shows it at the start of the next, so the GS rasterises while the EE builds the next frame, for one frame of input lag. 0 waits for the GS and flips at the end of each frame. Live. |
| `ps2_fb_dither` | 0 | 0 | Arch. | The GS's ordered dither, which hides the banding of a 16-bit framebuffer on gradients. Live. |
| `ps2_fb_16bit` | 1 | 1 | Arch. | 16-bit (5:5:5) framebuffers instead of 32-bit: 560 KB of VRAM each instead of 1120 KB, which the texture heap gets. Read once as the GS comes up, from `config.cfg` (or `+ps2_fb_16bit 0` on the command line), so a change applies on the next run. |
| `in_keyboard` | 1 | 1 | Arch. | The USB keyboard. Its IOP driver (usbd and ps2kbd) loads the first time this is on: at boot it is read from `config.cfg` ahead of time, so with it off there the driver never loads. Switching it off lets go of any keys the keyboard holds. Live. |
| `ps2_savedevice` | host | host | Arch. | Where save games go when running from the emulator's `host:` filesystem: `host` keeps them as QuakeSpasm does, `<gamedir>/<name>.sav` files; `mc` sends them to the memory card in MEMORY CARD slot 1, with a copy of `config.cfg`. On a console saves always go to the card, whatever this says. `ps2_saveinfo` shows where they are going. |
| `in_rumble` | 1 | 1 | Arch. | The pad's vibration: bursts for the player's shots, damage taken (stronger the harder the hit), pickups and powerups coming on. Off in menus, the console, demos and while paused. Live. |
| `in_rumbledebug` | 0 | 0 | | Prints each rumble effect as it starts, with its motor speeds and durations. |
| `ps2_mip_filter` | bilinear | bilinear | Arch. | How the walls filter, as QuakeSpasm's `gl_texturemode` does: `nearest`, `bilinear` (between texels, the nearest mip level) or `trilinear` (between levels too, at twice the texture reads). Live. |
| `ps2_mip_bias` | 0 | 0 | Arch. | Shifts the mip levels the walls sample, in levels; positive is blurrier. The GS picks a level by depth alone, so floors seen at a grazing angle come out sharper than their texel density calls for. Live. |
| `ps2_debug_overlays` | 1 | 0 | Arch. | Master switch for the debug panels below, all but the FPS counter. Each panel also has its own switch. |
| `ps2_show_fps` | 1 | 0 | Arch. | FPS counter, top right: green while every frame made its vsync, yellow once one missed, red at half the refresh rate or below. |
| `ps2_show_profile` | 1 | 0 | Arch. | Frame times per profiled event, under the FPS counter. Draws in profile builds only. |
| `ps2_show_memstats` | 1 | 0 | Arch. | Heap use per memory tag, with the total, its peak and what is left, bottom right. |
| `ps2_show_vramstats` | 1 | 0 | Arch. | GS texture heap use, resident textures, and this frame's uploads and forced GS drains, bottom left. |
| `ps2_show_drawstats` | 1 | 0 | Arch. | What the last frame submitted (triangles, particles, batches, clipping) and the DMA chain's use, top left. Draws in profile builds only. |
| `ps2_frame_log` | 0 | — | | Writes per-frame timings and draw counters to stdout as CSV `FLOG` rows, for `src/tools/scripts/frame_log/`. The `FLOG#open` lines naming each file opened are only written during a `ps2_perftest` run or the perf pass of `ps2_testmaps`. Registered in profile builds (`PS2_QUAKE_PROFILE`). |
| `ps2_testmaps` | 0 | — | | Loads every level in the game data's paks: the campaign in play order (start, each episode, end; the shareware pak0 has start and episode 1, the registered pak1 the rest), then any others alphabetically (pak1's deathmatch maps). It starts by saying how many it found and which campaign maps the data lacks, and prints what each level costs as it leaves it: QuakeSpasm's hunk and cache and what is left of it, the backend's level data by memory tag, the heap's peak and the RAM still free. **1**, the memory pass, stays `ps2_testmaps_dwell` seconds in each and ends with `MapCycle: done.`; `ps2_testmaps_restart` runs it again. **2**, the perf pass (it also needs a profile build), sets up a capture as `ps2_perftest` does, then in each level turns `god` and `notarget` on and turns a full circle (2 degrees a frame) at up to `ps2_testmaps_views` viewpoints, each after an `FLOG#view` marker; it quits at the end. Don't combine with `ps2_perftest`. |
| `ps2_testmaps_dwell` | 8 | — | | Seconds the memory pass of `ps2_testmaps` stays in each map once it has loaded. |
| `ps2_testmaps_views` | 5 | — | | Viewpoints the perf pass of `ps2_testmaps` visits in each level: where the player spawns, then each time the intermission camera, deathmatch start or teleporter exit farthest from those already picked. Up to 16; each takes about 3 seconds. |
| `ps2_perftest` | 0 | 0 | Arch. | An unattended performance capture: stops the attract loop, plays demo1, demo2 and demo3 once each with `ps2_frame_log 1`, the file-open notes on and the debug panels off, then quits. Arm it in `config.cfg` or `autoexec.cfg`; it sets itself back to 0 before quitting. Runs in profile builds only (`PS2_QUAKE_PROFILE`), but every build registers it, as it is archived. |
| `ps2_testcube` | 0 | — | | Draws the VU1 test cube on top of every frame: the GS/VU1 smoke test. |
| `ps2_testcube_tess` | 8 | — | | The test cube's tessellation per face, 1 to 8. Higher values push a face past one VU1 batch, to exercise chunked submission. |
| `ps2_testcube_vram_tex_eviction` | 0 | — | | Slides the cube's faces through the debug textures every 2 s, to exercise VRAM eviction. Use it with the heap shrunk (`kDebugHeapLimitWords` in `vram.cpp`). |
| `ps2_testcube_vulerp` | 0 | — | | Draws the cube through the keyframe-lerp path the alias models use instead, as a check of the VU1 unpack and lerp. |

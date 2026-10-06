# PS2 backend cvars

Every cvar registered by the PS2 backend under [src/ps2/](src/ps2/), with its default in the
debug (`make`) and release (`make release`) builds. See the [README](README.md) for the
bigger picture. QuakeSpasm's own cvars are not listed here, including the ones the backend
registers in place of the QuakeSpasm files it replaced (`gamma`, `r_lerpmodels`, ...).

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
| `ps2_frame_log` | 0 | — | | Writes per-frame timings and draw counters to stdout as CSV `FLOG` rows, for `src/tools/scripts/frame_log/`. Registered in profile builds (`PS2_QUAKE_PROFILE`). |
| `ps2_testcube` | 0 | — | | Draws the VU1 test cube on top of every frame: the GS/VU1 smoke test. |
| `ps2_testcube_tess` | 8 | — | | The test cube's tessellation per face, 1 to 8. Higher values push a face past one VU1 batch, to exercise chunked submission. |
| `ps2_testcube_vram_tex_eviction` | 0 | — | | Slides the cube's faces through the debug textures every 2 s, to exercise VRAM eviction. Use it with the heap shrunk (`kDebugHeapLimitWords` in `vram.cpp`). |
| `ps2_testcube_vulerp` | 0 | — | | Draws the cube through the keyframe-lerp path the alias models use instead, as a check of the VU1 unpack and lerp. |

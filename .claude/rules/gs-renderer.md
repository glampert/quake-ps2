---
paths:
  - "src/ps2/renderer/**"
---

# GS renderer facts

The README's "Rendering" section is the architecture overview. QuakeSpasm's renderer surface
is implemented by `vid.cpp` (GS bring-up, the `GL_BeginRendering`/`GL_EndRendering` frame
bracket, the screenshot readback), `draw.cpp` (`Draw_*`), `texmgr.cpp` (`TexMgr_*`) and
`refresh.cpp` (`R_*`). Under them, from the Quake II port: `render_system.*` (`ps2::rs`: VIF1
chains, batches, `DrawTriangles`, `Submit`), `cmd_buffer.*`, `gs.*` (GS front-end, register
values, 2D, readback), `vram.*` (texture heap), `texture.*` (`ps2::tex`), `scrap_atlas.*`,
`splash_screen.*` (the boot splash, drawn before the renderer exists), `vu1.*` (VU memory
layout, microprogram declarations). The 3D view: `view.cpp` (`R_RenderView`:
the world, the entity passes, the glows), `brush.cpp` (per-surface draw data, built by
`R_NewMap`), `lightmap.cpp`, `alias.cpp` (MDL models), `sprite.cpp`, `particles.cpp` and
`sky.cpp`. VU1 clips everything: the Quake II port's EE clipper (`clip.*`) went with its sky.

- **Set the mip constant every frame** (`rs::SetTextureSampling`, from the view's focal
  length; see `view.cpp`'s `SetUpTextureSampling`). Its default of 0 has the GS pick levels by
  log2 of the depth alone, and the walls come out blotchy, sampled from their smallest levels.
- **Five fixed CLUTs, one per 8-bit `tex::PixelFormat`**: Quake's palette (255 transparent),
  no-bright (224-255 black), fullbright-only (0-223 alpha 0, the rest at 0x80 for additive), the
  0..1 alpha ramp (particles) and the 0..255 light ramp (lightmaps, overbright). Walls sample
  with RGB components, so the palette's transparent 255 doesn't cut them.
- **A model skin's glow texture is DECAL with RGB components**: it adds its texels as they
  are, whatever colour the vertices carry - on the alias path the vertex colour is the shade -
  at the vertex alpha. So the fullbright pass is the model's own vertices drawn again
  (`rs::Resubmit`), Additive, with the entity's alpha in the light's .w. The walls' glow
  textures stay MODULATE, at the modulate identity.
- **Only one stream may claim the command buffer at a time.** An alias model draws through a
  `LerpStream` of its own, so the view submits its `TriangleStream` before each alias model
  (`rs::Submit` is free when the stream is empty).
- **A stream holds its transform by pointer**, so a pass that gathers under a local matrix (a
  brush entity's) must submit before the matrix goes out of scope; a later local at the same
  address would otherwise draw the earlier vertices under the new matrix.
- **The z-test has no "farther" (LEQUAL in the GS's inverted depth)**: NEVER, ALWAYS, GEQUAL
  and GREATER only. QuakeSpasm's sky needs it (its layers draw where the sky surfaces' depth
  is), so sky.cpp draws the layers first, then the surfaces with `DrawFlags::DepthOnly` - a
  blend that keeps the framebuffer, `(0 - 0) * As + Cd`, with depth writes on.

## Frame model: 2D and 3D interleave

- `SCR_UpdateScreen` draws the 3D view first (`V_RenderView`), then all of the 2D (`GL_Set2D`,
  tile clear, status bar, console, menus), all inside `GL_BeginRendering`/`GL_EndRendering`.
  The backend's test scene and debug overlays go last, in `GL_EndRendering`.
- **The loading screen draws frames of its own**, outside `SCR_UpdateScreen`, while the plaque
  is up: `rs::BeginFrame`, the plaque and its status bar, `rs::EndFrame(deferPresent=false)`
  (`DrawLoadingScreen` in vid.cpp), so each is on screen before the call returns. They skip
  `GL_BeginRendering`'s profiler rollover and the overlays, and nothing is drawn while a frame is
  open (`rs::IsFrameOpen`): a file read from inside a frame leaves the screen as it is.
- **Frames drawn while the save notice is up are shown at once** too: `GL_EndRendering`
  draws the notice over the engine's frame and passes `deferPresent=false` whatever
  `ps2_gs_latency` says, since the save device's access that follows draws nothing until it is
  done (`save/save_notice.h`). With the plaque up the notice is drawn on the loading screen
  instead. Before the renderer exists (the boot reads config.cfg off the card in `VID_Init`), the
  splash draws it: `splash::ShowImage` uploads the notice, composed on the EE as RGBA, to a fixed
  VRAM spot past the plaque's and draws it at twice its size; the environment's alpha test
  (`NOTEQUAL 0`) leaves out its see-through texels, so the splash's textures are RGBA now, the
  plaque's pixels unchanged (checked against its texels, 2026-10-20). `gs::Init` calls
  `splash::Retire` before laying out VRAM, after which the splash draws nothing.
- **So is `SCR_ModalMessage`'s question** ("start a new game?", Reset config's): it draws one
  frame with `scr_drawdialog` set, then waits for the answer drawing nothing, so a frame left for
  the next to show stayed off screen and the game seemed to hang. `GL_EndRendering` passes
  `deferPresent=false` for it (logged from a scratch build, 2026-10-21).
- **Framebuffer 0 is on screen from `gs::Init` until the first present** (the first frame is
  drawn into frame 1), showing whatever VRAM holds there. The boot splash draws the QUAKE plaque
  into exactly the default layout of frame 0 (640 wide, the standard's full height, 16-bit, at
  VRAM 0), so it stays up through `gs::Init` until then. For any other layout `gs::Init` clears
  frame 0 (`splash::IsShownIn`), and it turns the output on only once that is drawn. Checked in
  PCSX2 (2026-10-13): the plaque read back pixel for pixel after `gs::Init`, and frame 0 came back
  black with `ps2_video_mode pal` or `ps2_fb_16bit 0` on the NTSC BIOS.
- **Never `Con_Printf` between `GL_BeginRendering` and `GL_EndRendering`.** While the client
  isn't in a game (console, menus, loading), each `Con_Printf` redraws the screen through
  `SCR_UpdateScreen`, which would open a frame inside the frame (`rs::BeginFrame` asserts).
  `Con_DPrintf` and `Con_SafePrintf` hold that redraw off; `Sys_Printf` only logs.
- `BeginFrame` clears color+depth. 2D primitives accumulate in a lazily opened pending batch
  (ALLPASS z-test). `FlushPending2D` sends it and is a no-op when empty. **Every 3D emitter
  calls it first**, and `EndFrame` calls it last. Never defer all 2D to frame end: the 2D
  packet bakes texture VRAM addresses, and mid-frame 3D uploads can evict them. Don't
  reintroduce a frame-wide 2D bracket. Each 2D→3D switch is a GS sync point (~2 per frame).
- **TEST is not in the VU1 batch register block.** MIPTBP1 took its slot, and the 7-qword tag
  block can't grow: 8 qwords would need 2 more per double-buffer half, and there is 1. 3D
  relies on TEST holding the 3D pixel tests. The clear leaves it so, and `gs::EmitEnd2D`
  re-arms it whenever a 2D section closes. Any new path that writes TEST must restore it the
  same way.

## GS and libdraw

- **The TV standard is ours to pick, not libgraph's.** `graph_initialize` always takes the
  console's region (`graph_get_region()`: an `E` as the ROM name's 5th character is PAL) and
  has no override. So `gs::Init` makes its calls itself with the mode `ps2_video_mode` chose,
  through `system/video_mode.h`, which the error screen reads too. libgraph's interlaced FIELD
  mode shows 448 lines on NTSC and 512 on PAL (224/256 per field), and puts the picture at
  DX 652/DY 50 and 680/72. The framebuffer takes the standard's full height. A 640x512 16-bit
  framebuffer pair plus Z costs 240 KB more VRAM than 640x448: the texture heap drops from 2408
  to 2168 KB (measured here, 2026-10-13, as on the Quake II port). The present clock's field rate
  follows the chosen standard, not the console's. *(Q2)* Forcing NTSC on a PAL console works on
  hardware (2026-10-11): the perf test paced at 59.94 Hz.

- **VRAM readback** (`gs::DownloadFramebufferRows`, ps2sdk's own sequence): BITBLTBUF, TRXPOS,
  TRXREG, FINISH and TRXDIR=1 go down PATH2 (VIF1 DIRECT) behind MSKPATH3 and FLUSHA; wait for
  FINISH and an empty VIF1 FIFO, set `VIF1_STAT.FDR` and `BUSDIR`, receive by VIF1 DMA, then
  put both back and unmask PATH3. Nothing may be in flight (`rs::FinishFrameInFlight`), and
  the target is whole 64-byte lines (sync before, invalidate after). Works in PCSX2.
- **Alpha 0x80 = 1.0.** 0xFF is about 2× overbright under `(Cs-Cd)*As/128+Cd`. Scale
  engine-facing 0..255 alpha with `a >> 1`. In MODULATE, vertex colour 0x80 is identity.
- The palette CLUT gives opaque texels alpha **0xFF**, and MODULATE multiplies texel by vertex
  alpha over 0x80. So a blended textured draw puts a quarter of its 0..255 alpha on the vertex
  (`EmitTexturedRect`), not half as `EmitFillRect` does.
- **Texture uploads DMA straight out of `texture.pixels`** (REF tags), so the pixels must start
  16-byte aligned and be a whole number of quadwords. `draw_texture_transfer` sends
  `(w*h)>>4` qwords against a TRXREG of w x h, so a short tail leaves the GS waiting and the
  next GIF data becomes texels. A WAD lump at an arbitrary file offset can't be uploaded in
  place: draw.cpp copies pics into 64-byte-aligned blocks with rows padded to 16 texels, and
  keeps the real size in `srcWidth`/`srcHeight` for the draws.
- The ALPHA register computes `(A - B) * C + D` where **C is a scalar alpha** (As/Ad/FIX), not
  a colour. Colour × colour (GL's `GL_ZERO, GL_SRC_COLOR`) is not expressible. That is why
  lightmaps are alpha-only intensity atlases, with chroma per vertex. Check this before
  promising any two-texture multiply.
- libdraw's `draw_enable_blending()`/`draw_disable_blending()` toggle a **static read at
  primitive emission** (PRIM.ABE), not a register write. Set it per emission. A global enable
  makes opaque fills blend about 2× bright.
- `draw_setup_environment()` defaults to CLAMP wrap (program REPEAT afterwards where tiling
  is needed) and an alpha test of NOTEQUAL 0 that discards A==0 texels. That is how conchars
  transparency works (palette index 255 has alpha 0). `ATEST_KEEP_*` names what is
  *preserved*: `ATEST_KEEP_FRAMEBUFFER` == GS `ZB_ONLY`, which libdraw's `draw_enable_tests`
  uses, and under which a cut-out texel still writes depth: a fence texture's or a sprite's
  holes then hide what draws behind them later. 3D runs with `gs::MakePixelTests`
  (`ATEST_KEEP_ALL`) instead. Any value where 0 is meaningful, such as a black lightmap luxel,
  must clamp to 1, or it draws nothing.
- **SCISSOR doesn't clip HOST→LOCAL uploads.** Writes past the destination width wrap in VRAM
  (for a 640-wide PSMCT32 buffer, to `(x-640, y+32)`).
- **One XGKICK with many GIF tags: only the last tag may set EOP.** The GIF stops at the
  first EOP, so EOP on every fan's tag drew only the first fan. Intermediate tags get EOP=0.
  On the VU, remember the last tag's address and OR in `0x8000` after the loop. Keep PRE=1 on
  each tag so the GS starts a new fan.
- `draw_rect_textured` = 4 qwords per sprite. A full console of text is about 9K qwords.
- **libdraw's sprites cover their far corner.** `draw_rect_textured` and `draw_rect_filled` add
  `START_OFFSET` (2047.5625) to the near corner and `END_OFFSET` (2048.5625) to the far one, so a
  rectangle from x to x + w covers w + 1 pixels, and its texels spread over that many. The splash
  passes x + w - 1 and reads back exact 2x2 texels. `gs::EmitTexturedRect` passes x + w: every
  2D pic and glyph draws a column and a row wider than its rectangle, the last repeating the one
  before (the menus' 64x288 plaque covers 65x289, 5846 of its pixels off an exact 2x2 mapping;
  measured 2026-10-13). Changing it changes every 2D draw, so it waits for a decision.

## Texture coordinates and sizes

- **Normalized ST spans the TEX0 TW/TH extent, the image rounded *up* to a power of two**, not
  the image. Nearly every MDL skin is non-power-of-two (296x194 samples as 512x256). On screen
  this looks like a UV-flip bug, but it isn't one: GS and GL both have T=0 at the first row.
  Scale coordinates in [0,1] with `tex::StScaleFor()`, which takes the image's own size
  (`srcWidth`/`srcHeight`) over the extent. A *tiling* non-POT texture needs resampling on
  load (the walls are), not a coordinate scale. The 2D path uses `PRIM_MAP_UV` texel
  coordinates and is unaffected.
- **Keep S/T small: a console's GS loses precision on large values, and PCSX2 doesn't.** id's
  wall projection gives coordinates relative to the texture's world origin. *(Q2)* In base2 a
  face's largest |S| or |T| had a median of 13.5 repeats and a max of 160. On hardware some walls
  drew stretched and warped, and others swam as the camera moved; PCSX2's renderer showed none
  of it. Shifting each face's coordinates by whole repeats so the smallest lands in [0, 1) fixed
  them, confirmed on a console 2026-10-10, with the guard band at its usual 0.8. Here
  `brush.cpp`'s `BakeSurfacePolygon` does the same (`ps2_st_rebase`, 2026-10-13). Under REPEAT
  it samples the same texels, mip levels included. Anything new that sends large S/T is a
  candidate for the same artifacts.
- **Water had the same problem, and the loader can't shift it.** It arrives in raw texels, and
  the ripple's phase is a function of them. *(Q2)* On a console its texture wobbled as the camera
  turned, most visibly on the triangles the camera stood in (2026-10-11). So VU1 shifts the
  *output* by whole repeats, per triangle, after the warp (`WarpCorners` in
  textured_triangles.vcl); confirmed on hardware there. This port runs the same program
  (2026-10-13).
- **Skins and sprite frames are copied with their rows padded to 16 texels**, as draw.cpp's
  pics are: the upload sends `w*h` texels in whole quadwords, and 16 of the 61 shareware models
  have a `w*h` that isn't a multiple of 16 (308x149, 300x194, ...). The padding repeats each
  row's last texel. `width` is the padded stride, `srcWidth` the image's own.

## CLUTs and palettized textures

- Most textures are PSMT8, sampling Quake's palette. A second CLUT is an alpha ramp for
  coverage-only images (particles, lightmap atlases).
- **CSM1 reorder:** in every 32-entry group the two middle 8-entry blocks swap:
  `(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)`, which is its own inverse. If you
  forget it, colours come out scrambled but stable.
- A CLUT uploads as a 16x16 PSMCT32 image: exactly 4 blocks at CBP, dest width 64 (DBW=1).
  CBP is in blocks (word address >> 6).
- **CLD:** `gs::MakeTex0` binds indexed textures with `CLUT_COMPARE_CBP0` (CLD=4), and
  `gs::Init`'s `SeedClutBuffer` writes TEX2 with CLD=2 so CBP0 starts known. Invariants: every
  indexed TEX0 uses CLD=4 (one CLD=1 load leaves CBP0 stale), and CLUT contents never change
  after Init (the compare is on the address only). CLD is ignored for non-indexed formats.
- **TBW must be even for PSMT8/PSMT4** (128-px stride). Narrow 8-bit textures round their
  stride up for TEX0 and the upload (`TextureStridePixels()`).
- Page sizes: PSMT8 128x64, PSMCT32 64x32, PSMCT16/16S 64x64. libdraw's
  `draw_texture_transfer` takes `dest_width` separately, and DBW = dest_width >> 6, so a
  16-px transfer needs `dest_width >= 64`.

## VRAM and mipmaps

- **The GS addresses textures in 256-byte blocks, not pages.** Any block-aligned TBP is
  valid. PSMT8's block order equals PSMCT32's (PCSX2 `_blockTable8 == _blockTable32`), so a
  texture occupies exactly `[TBP, TBP + far-corner block + 1)`. Size by
  `vram::TextureFootprintWords`, never by whole pages. Block granularity cut city3's walls
  from 2024 KB to 1391 KB.
- Mip levels share the base allocation, first-fit by block (`vram::MipLayout`, one per POT
  size). Each level has its own TBP/TBW via MIPTBP1, and PSMT8 TBW stays even. TEX1.MTBA
  auto-layout only handles square sizes.
- Bilinear needs every level ≥ 8 texels per side: `MXL = min(3, log2(min(w,h)) - 3)`.
- LOD: LCM=0 gives `LOD = log2(1/Q) + K`. With Q = 1/view depth, `K = -log2(f)`,
  `f = (screenH/2)·cot(fovY/2)` = 320 at 640x448, fov 90. LOD is depth-only, so grazing
  floors under-filter (`ps2_mip_bias` tunes it).
- Cvars: `ps2_mipmaps` (load-time, 0 = old behaviour exactly), `ps2_mip_filter`
  nearest/bilinear/trilinear (live), `ps2_mip_bias` (live).
- Lightmap atlases are `ImageType::Wall` (Alpha8). Exclude them by format when a rule means
  "walls".
- Before changing the heap or residency policy, read the README's VRAM heap notes: LRU, never
  evict textures bound this frame, recoverable OOM, defrag on level change.

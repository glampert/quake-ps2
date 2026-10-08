---
paths:
  - "src/quake/**"
---

# QuakeSpasm's C (`src/quake`)

## Editing rules

- Keep QuakeSpasm's code as close to untouched as possible. Platform work belongs in `src/ps2`
  behind a seam. When an engine change is unavoidable, keep it minimal and tag it:
  `// [PS2_QUAKE]: <why>`. The first commit holds QuakeSpasm 0.97.0 as released, so
  `git diff <first commit> -- src/quake` shows every change.
- Built as C11 (`-std=gnu11`, because common.h's `q_min`/`q_max`/`CLAMP` use `_Generic` there
  and GCC 15 would otherwise default to C23), with `-fno-strict-aliasing
  -fsingle-precision-constant` and a lenient warning set. None of the backend's C++ style or
  `-Werror` rules apply here.
- **No double FPU on the EE.** Unsuffixed constants are made float by the flag. Don't add
  `double` math or `<math.h>` double calls to engine code: use `PS2Quake_Sinf`, `Cosf`,
  `Sqrtf`, `Floorf`, `Ceilf` and `Fabsf` from `ps2/math/math_c.h` (`mathlib.h` includes it),
  and newlib's float `atan2f`, `atanf` and `tanf`. QuakeSpasm's own `double` time (`realtime`,
  `cl.time`, `Sys_DoubleTime`) stays until profiling says otherwise.
- `Sys_Error` and `Host_Error` are `FUNC_NORETURN` here (unlike Quake II's `Sys_Error`).
- A changed `.c` here needs only `make`. There is no separate engine build.
- Files the PS2 doesn't build are deleted, not left behind. A file QuakeSpasm keeps only as
  declarations (`bgmusic.h`, `snd_codec.h`, `image.h`) stays because the backend defines what
  it declares.

## Seams the backend implements

QuakeSpasm has no renderer interface like Quake II's `refexport_t`: the client calls its GL
renderer directly. So the seam is that renderer's public surface, implemented in `src/ps2`:

- 2D: `draw.h` (`Draw_*`, `GL_SetCanvas`) and `GL_Set2D`.
- Frame: `GL_BeginRendering`/`GL_EndRendering`, `VID_*` and the `vid` global (`vid.h`).
- Refresh: `render.h` (`R_Init`, `R_NewMap`, `R_RenderView`, ...), `R_TranslatePlayerSkin`
  and `R_TranslateNewPlayerSkin`, `D_FlushCaches`, `Sky_*`, the renderer globals and cvars the
  client reads (`r_refdef`, `r_lerpmodels`, `gl_polyblend`, ...), and what `gl_model.c` calls
  as it loads: `GL_MakeAliasModelDisplayLists`, `GLMesh_DeleteVertexBuffers`,
  `GL_SubdivideSurface` (a no-op: the backend cuts the water itself).
- Textures: `gl_texmgr.h` (`TexMgr_*`).
- Platform: `Sys_*` (sys.h, plus the globals `isDedicated` and `sys_throttle`), `PL_*`
  (platform.h), `IN_*` (input.h), `SNDDMA_*` (q_sound.h), `CDAudio_*` (cdaudio.h), `BGM_*`
  with `bgmloop` and `bgm_extmusic` (bgmusic.h), and `net_drivers[]`/`net_numdrivers`
  (loopback only).

For the 3D renderer: `gl_model.h` pads `texture_t` to 80 bytes (the BSP texture's pixels
after it on the hunk start 16-byte aligned, which the GS uploads them in place from) and adds
`qmodel_t::ps2_render`, a brush or alias model's draw data; `gl_model.c` keeps all four mip
levels of a BSP texture and keeps the light samples one byte per luxel (no `.lit`);
`gl_rlight.c`'s `RecursiveLightPoint` reads them that way, and takes its texture coordinates in
float rather than ericw's double.

For the math: `mathlib.h` includes `ps2/math/math_c.h`, and every engine call to libm's `sin`,
`cos`, `sqrt`, `floor`, `ceil` and `fabs` is its `PS2Quake_*` version, tagged per function;
`atan`, `tan` and most `atan2`s are newlib's float versions. Three things stay double, on
purpose:

- `PF_vectoyaw` and `PF_vectoangles` keep `atan2`. They truncate to whole degrees, and on target
  `atan2f` put every vector at exactly 135 or 225 degrees one degree low, where double matches
  a PC build (all 40,960 axis and diagonal vectors tested, and all but one in each of two
  random sets of 200,000). QuakeC aims monsters with them, a few calls a second. Their square
  roots did go single, and matched the PC's pitch for all 216,384 vectors tested.
- `CalcSurfaceExtents`' `val` sum (ericw's comment says why); only the `floor`/`ceil` of its
  float result went single.
- `FloorDivMod`, which nothing calls since the software renderer went.

Arguments built from the double `cl.time` (the idle sway, the gun's angles,
`R_EntityParticles`) lost only their libm call: the double multiply in front of it waits for
the double-time work. `PS2Quake_Cosf`'s error grows with the argument (5e-6 at 50 radians),
which these tolerate. `PS2Quake_Floorf`/`Ceilf` match libm for every normal float; a denormal
input reads as zero, as the EE's FPU treats it anyway.

For the effects' random numbers: `common.h` adds `COM_FxRand`, MSVC's `rand()` inline (a
32-bit LCG returning 15 bits, the range the effects were written for), with its state in
`common.c`. `r_part.c`, the dynamic lights' flicker in `cl_main.c`, and the ricochet sounds and
lightning beams in `cl_tent.c` draw from it; everything else, the game's own randomness above
all, keeps newlib's `rand()`, a 64-bit LCG the R5900 runs as 40 instructions behind a call.
`CL_UpdateTEnts` reseeds both every frame: QuakeSpasm's `srand` (johnfitz's, to hold the beams
still while paused) and, beside it, `COM_FxRand`'s state from the same `cl.time`, run through a
multiplicative hash so that seeds a frame apart don't give the same first draw.

For sound: `snd_dma.c` defaults `snd_mixspeed` to 22050 instead of 44100 (the README's Sound
section says why), and `host.c` wraps the `S_Update` calls in the `SndMix` profile probe
(`ps2/debug/engine_profile.h`), the first of the engine probes to be placed.

For rumble: `view.c` calls `PS2_RumbleDamage` from `V_ParseDamage` and `PS2_RumblePickup` from
`V_BonusFlash_f`. For the memory reports: `zone.c` adds `Cache_UsedBytes` (the cache's list is
private to it), which `ps2_testmaps` prints.

For saves and settings (`save-games.md`): `host_cmd.c`'s `Host_Savegame_f` and
`Host_Loadgame_f`, and `menu.c`'s `M_ScanSaves`, read and write through the `PS2_Save*` hooks;
`host.c`'s `Host_WriteConfiguration` and `cmd.c`'s `Cmd_Exec_f` (for `config.cfg` only) through
`PS2_Config*`; and `M_Options_Key` writes the config on leaving the options menu.

Some files with GL names hold engine logic and stay, with only their GL halves cut:
`gl_model.c` (the server needs its BSP hulls and PVS), `gl_screen.c` (`SCR_UpdateScreen`, the
loading plaque, and `screenshot`, TGA only, over `PS2_ReadPixels`; it also defaults
`scr_menuscale`/`scr_sbarscale` to 2 for the 640x448 screen), `gl_refrag.c`, `gl_rlight.c`,
`r_part.c` (particle simulation) and `gl_fog.c` (its message parsing must run, or the stream
desyncs). `gl_warp.c` is gone: its subdivision fed a warp computed per texel, and the PS2's,
per vertex, wants a finer cut (brush.cpp makes it); the cvars it defined that the engine
registers live in the backend.

Two PS2 headers sit at the seam. `src/ps2/renderer/gl_types.h` gives quakedef.h the GL type
names QuakeSpasm's headers are written with, and nothing else: with no GL function declared,
a stray GL call fails to compile. `src/ps2/engine_hooks.h` declares what engine files call in
the backend that no QuakeSpasm header declares (the status bar's alpha and scissor, the view
blend, the screenshot readback, the PS2's additions to `default.cfg` that `cmd.c` runs right
after it) and the engine state the backend reads (`active_particles`).

## Known engine quirks

Record QuakeSpasm quirks here as they are found.

- `startdemos` (the last line of `quake.rc`) plays the attract loop only while `cl_startdemos`
  is 1 (the default, archived) or `-fitz` is given; otherwise it opens the main menu. Once a
  map is running (say from `autoexec.cfg`), it does neither.
- QuakeSpasm's own `default.cfg`, with gamepad binds (`LSHOULDER`, `RTRIGGER`, ...), is
  embedded as `default_cfg.h` but used only when no `default.cfg` exists. id's `pak0.pak` has
  one without them, so `Cmd_Exec_f` runs the backend's `PS2_DefaultConfig` (the pad's binds)
  right after any `default.cfg`. Binds made in `IN_Init` wouldn't last: it runs before
  `quake.rc`, and `default.cfg` opens with `unbindall`.
- The main menu's Quit exits at once (QuakeSpasm's "Quit now!"; the Y/N box only with
  `-fitz`), through `Host_Shutdown`, which writes `id1/config.cfg` and the console history,
  `history.txt`, in the user dir (on `host:`, `build/<config>/`).
- `Key_Event` prints "`<KEY>` is unbound, hit F4 to set." for an unbound key numbered 200 or
  more on every press, in menus too. Every pad key is one, which is why Circle, the menus'
  Back, has a default bind.
- Input is polled twice a frame: `Sys_SendKeyEvents` runs `IN_Commands` and
  `IN_SendKeyEvents`, then `Host_Frame` runs `IN_Commands` again. `SCR_ModalMessage` (the
  "start a new game?" prompt) and `Con_NotifyBox` loop on `Sys_SendKeyEvents` from inside a key
  handler, so the input layer re-enters itself: a key's state must be updated before its
  `Key_Event`, as in_sdl.c does.
- Typed text goes through `Char_Event`, already shifted (SDL2's text input), and only while
  `Key_TextEntry()` says the engine takes text. `Key_Event` takes keys by position, and keys.c
  has no `keyshift[]` table, unlike Quake II's.
- `Cmd_AddCommand` after `Host_Init` has finished is a `Sys_Error` ("Cmd_AddCommand after
  host_initialized"). Anything that registers commands must run inside `Host_Init`.
- `Host_Init` queues `vid_unlock` to run after the configs; the backend registers it as a
  no-op, since the PS2 has one fixed video mode.
- `VID_Init` runs inside `Host_Init`, before `quake.rc` executes `config.cfg`. A cvar that has
  to be right when the GS comes up (`ps2_fb_16bit`) is read from the file ahead of time with
  `CFG_ReadCvars`, as QuakeSpasm's own `VID_Init` does for its video mode.
- `Con_Printf` redraws the screen (`SCR_UpdateScreen`) for every line while the client isn't
  in a game. Backend code that can run inside a frame prints with `Con_DPrintf` or
  `Con_SafePrintf` instead (see gs-renderer.md).
- `+commands` on the command line do nothing with the shareware data: `stuffcmds` reads the
  `cmdline` cvar, which `COM_CheckRegistered` fills only for the registered version.
- `-dedicated` stops with "Network not available!": a dedicated server needs a network driver
  besides loopback, and the PS2 has none.
- `Host_Map_f` frees the hunk back to the host level (`Host_ClearMemory`) before loading the
  next map, so maps never overlap in the hunk.
- **`qmodel_t::cache` must stay its last field**: zone.c's `Cache_Free` finds the model from
  it with `(qmodel_t *)(c + 1) - 1`. New fields go before it (brush.cpp static_asserts it).
- A model's `type` is only set once its loader is done: while an alias model's skins go
  through `TexMgr_LoadImage`, it still reads `mod_brush`. texmgr.cpp tells walls from skins
  and sprites by the model's file name instead.
- `R_PushDlights` marks surfaces with `r_framecount + 1`, and the view increments
  `r_framecount` right after, so a surface lit this frame reads `dlightframe == r_framecount`.
  `R_NewMap` builds the lightmaps at frame 1, or every surface (dlight frame 0) would read as
  lit by a dynamic light.
- **Alias models live in the cache**, between the hunk's low and high marks: a load moves them
  (`Cache_Move`) and room for another evicts them (`Cache_Free`), which frees their textures
  through `TexMgr_FreeTexturesForOwner`. `Mod_Extradata` reloads an evicted one - mid-frame, if
  that is when it is drawn. So nothing the DMA reads after the draw may live there: a model's
  corners (alias.cpp) are on the heap for as long as its `qmodel_t`, its skins are copies, and a
  texture freed mid-frame is held until that frame has drawn (texmgr.cpp). The poses stay in the
  cache, read by the EE as the model draws. `aliashdr_t` data is 8-byte aligned there (a 56-byte
  cache header ahead of it), so never assume 16.
- `Mod_LoadAliasModel` calls `GL_MakeAliasModelDisplayLists` on every load of a model, its
  working arrays (`stverts`, `triangles`, `poseverts`) still holding it, before it copies the
  model into the cache: what it puts on the hunk then (the poses, in `hdr->vertexes`) goes with it.
- QuakeSpasm writes `vid_restart` at the end of every `config.cfg`. The backend registers it as
  a no-op, as it does `vid_unlock`.
- QuakeSpasm keeps the entity lerp times (`lerpstart`, `movelerpstart`) in float, against the
  double `cl.time`; alias.cpp converts `cl.time` once per model and does that math in float.

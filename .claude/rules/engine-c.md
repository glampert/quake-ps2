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
  `double` math or `<math.h>` double calls to engine code: use `PS2_Sinf`, `Cosf`,
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
- Frame: `GL_BeginRendering`/`GL_EndRendering`, `VID_*` and the `vid` global (`vid.h`), and the
  `vid_menu*fn` hooks `menu.c` draws Options' Video Options page through (vid.cpp fills them in
  `VID_Init`; null, as menu.c defines them, the row stays hidden).
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
`cos`, `sqrt`, `floor`, `ceil` and `fabs` is its `PS2_*` version, tagged per function;
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

`anglemod` divides by its 360/65536 step where QuakeSpasm multiplies by 65536/360: in float that
constant folds low, and the view turned clockwise a step a frame (ps2-platform.md has why).
`CL_AdjustAngles` runs it on the yaw every frame the client is connected, whatever the input.

Arguments built from the double `cl.time` (the idle sway, the gun's angles,
`R_EntityParticles`) lost only their libm call: the double multiply in front of it waits for
the double-time work. `PS2_Cosf`'s error grows with the argument (5e-6 at 50 radians),
which these tolerate. `PS2_Floorf`/`Ceilf` match libm for every normal float; a denormal
input reads as zero, as the EE's FPU treats it anyway.

For the clocks: `realtime`, `cl.time`, `sv.time` and `host_frametime` stay double. `realtime`
counts from boot, and as a float it would lose frame precision within hours. What cost was
comparing float times against them once per item, each a soft-float call, so those loops read
the clock into a float once: `CL_RunParticles` (each particle's `die`), `CL_DecayLights`,
`R_PushDlights` and `CL_ReadFromServer`'s dlight count, and `SV_Physics`'s `nextthink` test,
which only picks the lerp interval sent to the client (its `svtime` is `volatile`: GCC otherwise
keeps `sv.time` across the loop's calls and converts it again for every entity). `SV_RunThink`
and `SV_Physics_Pusher` still compare in double: they decide when QuakeC thinks run, and float
could move one by a frame.

For collision: `world.c`'s `SV_HullPointContents` and `SV_RecursiveHullCheck` are back to id's
float `DotProduct`, where QuakeSpasm uses `DoublePrecisionDotProduct` against players sticking in
walls. On the EE the double version was 14 soft-float calls a hull node, and it held e4m7 to 30
fps (performance.md has the numbers). A stuck spot that turns up gets fixed where it is, not by
going back to double. A scripted walk (forward into walls, strafe, back, jump) at a fixed 20 fps
ended in the same places on e1m1 and e4m7 in both versions, and within 5 units on e2m2.

For the effects' random numbers: `common.h` adds `COM_FxRand`, MSVC's `rand()` inline (a
32-bit LCG returning 15 bits, the range the effects were written for), with its state in
`common.c`. `r_part.c`, the dynamic lights' flicker in `cl_main.c`, and the ricochet sounds and
lightning beams in `cl_tent.c` draw from it; everything else, the game's own randomness above
all, keeps newlib's `rand()`, a 64-bit LCG the R5900 runs as 40 instructions behind a call.
`CL_UpdateTEnts` reseeds both every frame: QuakeSpasm's `srand` (johnfitz's, to hold the beams
still while paused) and, beside it, `COM_FxRand`'s state from the same `cl.time`, run through a
multiplicative hash so that seeds a frame apart don't give the same first draw.

For sound: `snd_dma.c` defaults `snd_mixspeed` to 22050 instead of 44100 (the README's Sound
section says why).

For the profiler (`ps2/debug/engine_profile.h`, compiled out of release builds): `host.c` wraps
`Host_ServerFrame` (`Server`), `CL_RunParticles` (`ClParticles`) and the `S_Update` calls
(`SndMix`) in probes, and `cl_main.c`'s `CL_ReadFromServer` its message loop (`ClParse`) and
`CL_RelinkEntities`/`CL_UpdateTEnts` (`ClScene`). `common.c`'s `COM_FindFile` names each file it
opens (not `COM_FileExists`' lookups) through `PS2_FileOpening` (engine_hooks.h): to the frame log,
which writes it only during a perf run, and to the loading screen. The reads themselves are timed
in the backend's `Sys_File*` (`FsIo`).

For loading (`engine_hooks.h`, `ps2/debug/load_trace.h`): `gl_screen.c` calls
`PS2_LoadingScreenBegin` once the plaque is up in `SCR_BeginLoadingPlaque` (after its early
returns), and `PS2_LoadingScreenEnd` in `SCR_EndLoadingPlaque` and at `SCR_UpdateScreen`'s
timeout. The load trace's points, compiled out unless `PS2_QUAKE_LOAD_TRACE` is 1, sit beside
QuakeSpasm's own `Con_DPrintf`s in `SV_SpawnServer` (sv_main.c), `CL_ParseServerInfo` (cl_parse.c)
and `CL_SignonReply` (cl_main.c).

For the compressed packs (`engine_hooks.h`, `ps2/fs/pkz_pack.cpp`): `common.c`'s
`COM_AddGameDirectory` loads a `pakN.pkz` in place of `pakN.pak` (`COM_LoadPkzFile`),
`COM_FindFile` opens a pkz's files with `PS2_PkzOpenHandle` and `PS2_PkzOpenStream`, and
`COM_Game_f` closes one with `PS2_PkzClose`. `common.h`'s `pack_t` gains `pkz`: a pkz's `handle`
is -1, and its `packfile_t.filepos` is a file's index, not an offset. The handle is a stream the
backend put in `sys.cpp`'s table (`ps2::sys::OpenFileHandle`), which `COM_CloseFile` closes like a
loose file's, as no pack has it. Every pkz stream starts at position 0, where a pak's starts at its
file's offset in the pak: QuakeSpasm's `FS_*` readers take `ftell` at the open and seek relative
to it, and the demo reader only reads on, so anything new that reads a `COM_FOpenFile` stream must
do the same, and never seek past the file's end. `COM_LoadPkzFile` keeps only the file count of
`COM_LoadPackFile`'s test for id's pak0: its CRC is of the pak's directory bytes, which a pkz
doesn't keep, so a pkz of the shareware pak0 still plays as the shareware version.

For the background loader (`ps2/fs/async_io.h`, `async_io_c.h`): `COM_LocateFile` (common.c,
declared in common.h) finds a file without reading it: a `.pak` gets a second descriptor
(`pack_t::asyncFd`, opened on first use, closed in `COM_Game_f` after `PS2_AsyncIoDrain`), a pkz
entry comes back as its compressed range on the pack's reader descriptor (`PS2_PkzLocate`), a loose
file as its own stream's `fileno`, held open until `COM_ReleaseLocation`. Missing is -1 as from
`COM_FindFile`; found with `fd` -1 means "load synchronously". It calls `PS2_FileOpening` like an
open, so the perf run's `FLOG#open` and the loading screen see it. **A `.dem` opened through
`COM_FOpenFile` is a read-ahead stream** (`PS2_WantsReadAheadStream`/`PS2_OpenReadAheadStream`,
`ps2/fs/readahead_stream.h`): an unbuffered `fopencookie` stream over a 64 KB ring the loader
fills, unless the file is a deflated pkz entry, `ps2_readahead_kb` is 0 or the ring can't be
allocated, when it gets the plain stream as before. cl_demo.c is untouched: `fscanf`, `fgetc`,
`fread` and `fclose` work on it (newlib pushes `fscanf`'s look-ahead back into the stream's own
one-byte buffer, never into the cookie).

For the sounds: `snd_mem.c`'s `S_LoadSound` returns NULL while `PS2_SoundLoadPending` and hands a
sound to `PS2_SoundLoadAsync` (`ps2/audio/sound_load.h`) unless `snd_precaching` (snd_dma.c's
`S_BeginPrecaching`/`S_EndPrecaching`, the stubs QuakeSpasm left, around `CL_ParseServerInfo`'s
pass); the half after the read is `S_FinishLoadSound`, which the pump reaches through
`PS2_SoundFinishLoad`. `snd_dma.c`'s `S_StartSound` gives a pending sound to
`PS2_SoundStartDeferred` instead of dropping it, and `S_Shutdown` drains first. `gl_model.c`'s
`Mod_ClearAll` (every level change, from `Host_ClearMemory`) calls `PS2_AsyncIoDrain`.

For the models: `gl_model.c`'s `Mod_LoadModel` is split at its read, the half after it being
`Mod_LoadModelFromBuffer`, which the pump reaches through `PS2_ModelFinishLoad` (a no-op when the
model is in the cache again by then, since `Cache_Alloc` of an allocated cache is a `Sys_Error`).
`Mod_ExtradataAsync` is `Mod_Extradata` for a draw: for an alias model with no cache it asks
`PS2_ModelLoadAsync` (`ps2/renderer/model_load.h`) and returns NULL while `PS2_ModelLoadPending`,
falling back to `Mod_Extradata` when the loader can't take the file. Only alias.cpp's
`DrawAliasModel` uses it; `R_TranslateNewPlayerSkin` and `PrintFrameName` keep `Mod_Extradata`.

For rumble: `view.c` calls `PS2_RumbleDamage` from `V_ParseDamage` and `PS2_RumblePickup` from
`V_BonusFlash_f`. For the memory reports: `zone.c` adds `Cache_UsedBytes` (the cache's list is
private to it), which `ps2_testmaps` prints.

For the debug page (`engine_hooks.h`, `ps2/debug/debug_menu.cpp`): `menu.h` adds `m_ps2debug` to
`m_state_e`, and `menu.c` an `OPT_PS2DEBUG` row ("Debug Options") ahead of `OPT_VIDEO`, which
stays last as its hiding logic needs, and hands the page its frames and keys in `M_Draw` and
`M_Keydown`. Options' own "Goto console" row (`OPT_CONSOLE`) is gone: the debug page's "Open
console" row does it.

For the main menu (`engine_hooks.h`, `ps2/menu/about_menu.cpp`): `M_Main_Draw` draws
`gfx/mainmenu.lmp` a 20-row item at a time with `PS2_DrawPicRows` (draw.cpp), the Multiplayer row's
line commented out, and `gfx/menuabout.lmp` (the ELF's, served by `Draw_CachePic`) in the slot
after Help/Ordering; `M_Main_Key`'s cases follow, with `M_Menu_MultiPlayer_f`'s commented out, and
the About item opens the backend's page (`m_ps2about` in `menu.h`, handed its frames and keys in
`M_Draw` and `M_Keydown` as the debug page is). The multiplayer menus' code and `menu_multiplayer`
stay. The strips' bounds keep each item whole: Help/Ordering's g reaches row 80, and Single
Player's strip takes the see-through row under it, so a sprite's repeated last row
(gs-renderer.md) lands on nothing.

For the pad in the menus (`engine_hooks.h`, `ps2/input/input.cpp`): the Options rows QuakeSpasm
gives the mouse work the look stick. `OPT_MOUSESPEED` is Look Speed (`PS2_LookSpeed`/
`PS2_SetLookSpeed`, `joy_sensitivity_yaw` and `_pitch` together, 0.5 to 2), `OPT_INVMOUSE` Invert
Look (`joy_invert`), `OPT_ALWAYSMLOOK` Stick Look (QuakeSpasm's own `+mlook` toggle: `IN_Move`
lets the look stick look up and down only under it, and without it the stick only turns and
leaves the pitch drifting, as in_sdl.c's `IN_MouseMove` treats the mouse), `OPT_MUSICEXT`
Background Music. `OPT_LOOKSTRAFE` is commented out (the mouse strafing instead of turning; a look
stick that did would leave nothing to turn with), and `OPT_PS2RUMBLE` steps `in_rumble`.
`PS2_DefaultConfig` adds `+mlook`, as QuakeSpasm's own default.cfg does, so `host.c`'s
`Host_WriteConfiguration` writes `-mlook` when it is off: with only QuakeSpasm's `+mlook` line,
the defaults would turn it back on at every boot.
The Controls page lists the pad's buttons only, by `PS2_PadKeyName`'s names: `M_FindKeysForCommand`
and `M_UnbindCommand` skip every other key, the bind grab takes only a pad button, Square
(`K_XBUTTON`) clears beside Backspace, and the header says so. `bindnames`' `+mlook` and `+klook`
read "stick look" and "d-pad look". The binds `config.cfg` keeps still name the keys as keys.c
does (`ABUTTON`); only the page shows `Cross`. `SCR_ModalMessage`'s two questions (New Game over a
running game, Reset config) say "Cross: yes   Circle: no" for the keyboard's "(y/n)", and
`gl_screen.c`'s `SCR_DrawNotifyString` draws them in a message box, as the save notice is, no
wider than the 320-unit canvas: the save code's failure boxes go through it too.

For saves and settings (`save-games.md`): `host_cmd.c`'s `Host_Savegame_f` and
`Host_Loadgame_f`, and `menu.c`'s `M_ScanSaves`, read and write through the `PS2_Save*` hooks;
`host.c`'s `Host_WriteConfiguration` and `cmd.c`'s `Cmd_Exec_f` (for `config.cfg` only) through
`PS2_Config*`; `M_Options_Key` writes the config on leaving the options menu; and the Options
page's Save Device row (`OPT_PS2SAVEDEVICE`, ahead of `OPT_PS2DEBUG`) shows and steps
`ps2_savedevice` through `PS2_SaveDeviceName`/`PS2_SaveDeviceToggle`. `M_Save_Draw` and
`M_Load_Draw` end with `PS2_SaveMenuDrawProblem` (the box saying why the save device can't be
used), and `M_Save_Key`/`M_Load_Key` scan again (`M_ScanSaves`) instead of taking a slot while
`PS2_SaveMenuProblem` says so. `M_Load_Key` calls `PS2_SaveLoadStarting` before it raises the
loading plaque: the save notice and the music's fade-out have to come first, as the plaque's
`S_StopAllSounds` would cut the music off. The notice is drawn in `menu.c`'s `M_DrawTextBox`,
which `engine_hooks.h` declares (menu.h doesn't), as it does `gl_screen.c`'s `scr_initialized`.
`M_ScanSaves` brackets its loop with `PS2_SaveScanBegin`/`PS2_SaveScanEnd`, one notice for the
twenty slots it reads. `M_Draw`'s "Quit now!" queues `quit` for the next frame instead of calling
`Host_Quit_f` from inside this one: the config.cfg written on the way out can put the notice up,
which draws a frame of its own.

Some files with GL names hold engine logic and stay, with only their GL halves cut:
`gl_model.c` (the server needs its BSP hulls and PVS), `gl_screen.c` (`SCR_UpdateScreen`, the
loading plaque, and `screenshot`, TGA only, over `PS2_ReadPixels`; it also defaults
`scr_menuscale`, `scr_conscale` and `scr_sbarscale` to 1.5 for the 640x448 screen, where
QuakeSpasm has 1, and `scr_sbaralpha` to 0.5, where it has 0.75, and gives the loading plaque
150 s before `SCR_UpdateScreen` prints "load failed." and draws again, where QuakeSpasm gives it
60: the plaque spans every frame of a load, and the Quake II port's
loads off USB on a console ran past two minutes), `gl_refrag.c`, `gl_rlight.c`, `r_part.c` (particle simulation) and `gl_fog.c` (its message
parsing must run, or the stream desyncs). `gl_warp.c` is gone: its subdivision fed a warp
computed per texel, and the PS2's, per vertex, wants a finer cut (brush.cpp makes it); the
cvars it defined that the engine registers live in the backend.

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
  `-fitz`), through `Host_Shutdown`, which writes `id1/config.cfg`. QuakeSpasm's console
  history file, `history.txt` in the user dir, is neither read at boot nor written on exit here
  (`History_Init`, tagged; 2026-10-14).
- `Key_Event` prints "`<KEY>` is unbound, hit F4 to set." for an unbound key numbered 200 or
  more on every press, in menus too. Every pad key is one, which is why Circle, the menus'
  Back, got a default bind. Here it names the pad's button and the Controls page ("Square is
  unbound: bind it in Options, Controls.", tagged) and keeps quiet while a menu is up, where
  Circle goes back and Square clears a bind.
- Input is polled twice a frame: `Sys_SendKeyEvents` runs `IN_Commands` and
  `IN_SendKeyEvents`, then `Host_Frame` runs `IN_Commands` again. `SCR_ModalMessage` (the
  "start a new game?" prompt) and `Con_NotifyBox` loop on `Sys_SendKeyEvents` from inside a key
  handler, so the input layer re-enters itself: a key's state must be updated before its
  `Key_Event`, as in_sdl.c does.
- `SCR_ModalMessage` draws its question in one frame (`scr_drawdialog`), then waits for the answer
  drawing nothing. With `ps2_gs_latency` that frame was left for the next to show, so New Game
  over a running game showed nothing and seemed to hang; `GL_EndRendering` shows it at once now.
  Its `Key_BeginInputGrab` lets go of every held key (`Key_ClearStates`), so a pad button still
  held repeats as a fresh press: Cross held half a second on New Game answered yes. A pad key
  whose `keydown[]` the engine cleared doesn't repeat until let go (`UpdatePadKey`).
- `M_AdjustSliders`' mouse look row queues `+mlook`/`-mlook` with `Cbuf_AddText` and no newline:
  behind whatever a script still has queued (its `wait`s hold the rest of it), so a scripted
  toggle runs only once the script ends. Scripts type `-mlook` themselves.
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
  that is when it is drawn; since 2026-11-01 the draw uses `Mod_ExtradataAsync`, which has the
  background loader read it back and skips the entity meanwhile (`flush` in a level shows it:
  `model: progs/v_shot.mdl reloaded in the background, 23 ms after it was asked for`). So nothing the DMA reads after the draw may live there: a model's
  corners (alias.cpp) are on the heap for as long as its `qmodel_t`, its skins are copies, and a
  texture freed mid-frame is held until that frame has drawn (texmgr.cpp). The poses stay in the
  cache, read by the EE as the model draws. `aliashdr_t` data is 8-byte aligned there (a 56-byte
  cache header ahead of it), so never assume 16.
- `Mod_LoadAliasModel` calls `GL_MakeAliasModelDisplayLists` on every load of a model, its
  working arrays (`stverts`, `triangles`, `poseverts`) still holding it, before it copies the
  model into the cache: what it puts on the hunk then (the poses, in `hdr->vertexes`) goes with it.
- QuakeSpasm writes `vid_restart` at the end of every `config.cfg`. The backend registers it as
  a no-op, as it does `vid_unlock`.
- `SV_SendReconnect` runs `reconnect` on the local client at once, and sends it as a stufftext
  too. In play the second copy runs mid-handshake, where resetting the signon is harmless. In a
  script whose `wait`s hold the command buffer past a `changelevel`, it runs only when the waits
  run out, after the level is up: the client drops to signon 0 under the plaque and stays there
  until the 150 s timeout (seen 2026-10-13). End such a script with the `changelevel`, or wait
  only before it.
- QuakeSpasm keeps the entity lerp times (`lerpstart`, `movelerpstart`) in float, against the
  double `cl.time`; alias.cpp converts `cl.time` once per model and does that math in float.

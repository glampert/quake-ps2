/* ================================================================================================
 * File: engine_hooks.h
 * Brief: What QuakeSpasm's C needs from the backend that no QuakeSpasm header declares: the PS2
 *        stand-ins for the few OpenGL calls made outside the renderer (the status bar's alpha
 *        and scissor, the view blend, the screenshot readback), the loading screen, the
 *        compressed packs, the debug page, the PS2's additions to the default config, where save
 *        games and config.cfg are kept, and the engine state and functions the backend uses that
 *        QuakeSpasm keeps to one file. Every engine line that uses this is tagged [PS2_QUAKE].
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_ENGINE_HOOKS_H
#define PS2_ENGINE_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "quake/quakedef.h" // qpic_t, particle_t

// ------------------------------------------------------------------------------------------------
// 2D (sbar.c)
// ------------------------------------------------------------------------------------------------

// Draw_Pic at 'alpha' opacity, 0 to 1: what Sbar_DrawPicAlpha did by wrapping Draw_Pic in GL
// blend state.
void PS2_DrawPicAlpha(int x, int y, const qpic_t * pic, float alpha);

// Clips the 2D draws that follow to a rectangle, in screen pixels, until PS2_ResetScissor2D:
// Sbar_DrawScrollString's glScissor. The origin is the top-left corner; the one caller clips to
// the full screen height, so GL's bottom-left origin would give the same rectangle.
void PS2_SetScissor2D(int x, int y, int width, int height);
void PS2_ResetScissor2D(void);

// ------------------------------------------------------------------------------------------------
// View (view.c)
// ------------------------------------------------------------------------------------------------

// V_PolyBlend's full-screen tint: damage, powerups, underwater. 'rgba' is v_blend, each 0 to 1.
void PS2_DrawPolyBlend(const float rgba[4]);

// The pad's rumble for what view.c hears about the player (src/ps2/input/rumble.cpp): damage
// taken, from V_ParseDamage, with the armor and health it cost; and a pickup, from the bonus
// flash ("bf") every item's touch sends.
void PS2_RumbleDamage(int armor, int blood);
void PS2_RumblePickup(void);

// ------------------------------------------------------------------------------------------------
// Screenshots (gl_screen.c)
// ------------------------------------------------------------------------------------------------

// The last finished frame, read back from GS VRAM as 8-bit RGB with the top row first: glwidth *
// glheight * 3 bytes. SCR_ScreenShot_f's glReadPixels.
void PS2_ReadPixels(byte * rgb);

// ------------------------------------------------------------------------------------------------
// Loading (common.c, gl_screen.c)
// ------------------------------------------------------------------------------------------------

// A file COM_FindFile is opening to read, by its name in the search path (a COM_FileExists lookup
// isn't one): named in the frame log during a perf run, and on the loading screen.
void PS2_FileOpening(const char * fileName);

// The loading plaque went up (SCR_BeginLoadingPlaque) and came down (SCR_EndLoadingPlaque, or
// SCR_UpdateScreen's timeout). Nothing draws a frame in between, so the backend shows a loading
// screen of its own (renderer/loading_screen.h).
void PS2_LoadingScreenBegin(void);
void PS2_LoadingScreenEnd(void);

// ------------------------------------------------------------------------------------------------
// Compressed packs (common.c)
// ------------------------------------------------------------------------------------------------
//
// A pakN.pkz is loaded in place of the pakN.pak with its number: a zip of the same files, most of
// them deflated, made by pakzip (src/ps2/fs/pkz_format.h has the format). COM_AddGameDirectory
// looks for one before each pak and keeps it in a pack_t like a pak's, with the same packfile_t
// list for the lookups; these are the part that knows zips (src/ps2/fs/pkz_pack.cpp). A pkz reads
// its files itself, so its pack_t has no handle (-1) and its pkz set, and each packfile_t's
// filepos is the file's index in it. Only the main thread may use one.

typedef struct ps2_pkz_s ps2_pkz_t;

// Called by PS2_PkzListFiles once per file, in directory order, with the index the calls below
// take. 'name' is NUL-terminated and shorter than MAX_QPATH.
typedef void (*ps2_pkz_file_fn)(void * context, int index, const char * name, int lengthBytes);

// Opens the pack at 'path' and reads its directory; null if there is no such file. One that isn't
// a pack this build can read is a Sys_Error, as COM_LoadPackFile's damaged pak is.
ps2_pkz_t * PS2_PkzOpen(const char * path);

// How many files PS2_PkzListFiles will report: directories, and names of MAX_QPATH characters
// or more (with a warning), are left out.
int PS2_PkzNumFiles(const ps2_pkz_t * pkz);

// Reports every file to 'fn', then frees the copy of the zip directory PS2_PkzOpen kept for it.
// Call it once, right after PS2_PkzOpen.
void PS2_PkzListFiles(ps2_pkz_t * pkz, ps2_pkz_file_fn fn, void * context);

// File 'index', opened at its first byte as COM_FindFile opens a pak's: a Sys_File handle for
// COM_OpenFile, which Sys_FileRead reads (whole, in one inflate) and COM_CloseFile closes, since
// no pack has that handle; or a stdio stream for COM_FOpenFile. Positions start at 0, not at an
// offset in the pack as on a pak, which QuakeSpasm's FS_* readers allow for: they take ftell at
// the open and seek relative to it. 'name' is for messages. A handle that can't be opened is a
// Sys_Error, a stream null.
int PS2_PkzOpenHandle(ps2_pkz_t * pkz, int index, const char * name);
FILE * PS2_PkzOpenStream(ps2_pkz_t * pkz, int index, const char * name);

// Closes the pack (COM_Game_f, leaving a game directory). Streams still open on it keep it alive
// until they are closed.
void PS2_PkzClose(ps2_pkz_t * pkz);

// ------------------------------------------------------------------------------------------------
// The debug page (menu.c)
// ------------------------------------------------------------------------------------------------

// Options' "Debug Options" row opens the page (m_state m_ps2debug), and M_Draw and M_Keydown hand
// it its frames and keys while it is up (src/ps2/debug/debug_menu.cpp).
void PS2_DebugMenuOpen(void);
void PS2_DebugMenuDraw(void);
void PS2_DebugMenuKey(int key);

// ------------------------------------------------------------------------------------------------
// Configs (cmd.c)
// ------------------------------------------------------------------------------------------------

// Config text "exec default.cfg" runs right after the file: the gamepad's binds, which id's
// default.cfg doesn't have (QuakeSpasm's own default.cfg, in a quakespasm.pak the PS2 doesn't
// ship, adds some), and always run, for the sticks (see input.cpp). It runs whenever
// default.cfg does, so the options menu's "Reset to defaults" restores it too, and config.cfg,
// which quake.rc runs next, keeps the player's own settings.
const char * PS2_DefaultConfig(void);

// ------------------------------------------------------------------------------------------------
// Save games (host_cmd.c, menu.c)
// ------------------------------------------------------------------------------------------------
//
// QuakeSpasm keeps a save as <gamedir>/<name>.sav and reads and writes it through stdio. These
// take its place at the three spots that do: on a console the saves live on the memory card
// (src/ps2/save/), and running from host: they stay host files unless ps2_savedevice says "mc".
// `path` is always the <gamedir>/<name>.sav QuakeSpasm built.

// The stream Host_Savegame_f writes a save to, having said where it goes. Null if it can't be
// written, with the reason on the console. For the memory card, the card notice is up and the
// music faded out before it returns (src/ps2/save/card_notice.h).
FILE * PS2_SaveOpenWrite(const char * path);

// Closes that stream, which stores the save where it goes; `comment` is the line the menus list
// it by. False if it didn't make it, with the reason on the console. Takes the card notice down.
qboolean PS2_SaveCloseWrite(FILE * f, const char * comment);

// The save as text in a malloc block, as COM_LoadMallocFile_TextMode_OSPath returns it, having
// said where it is loading from. Null if there is none or it can't be read; then the load menu's
// loading plaque, if it is up, comes down too.
char * PS2_SaveLoadText(const char * path);

// The load menu is about to raise the loading plaque, which stops every sound at once, and then
// run "load". From the memory card, the card notice goes up and the music fades out now, and the
// load takes the notice down once the card has been read (src/ps2/save/card_notice.h).
void PS2_SaveLoadStarting(void);

// The comment line of a save, for the load and save menus. False if the slot is empty or can't
// be read.
qboolean PS2_SaveReadComment(const char * path, char * comment, int size);

// The options menu's Save Device row: where saves go now ("host files" or "memory card"), and a
// step to the other. Only a run from host: has a choice (ps2_savedevice); on a console saves
// always go to the card, and the step does nothing.
const char * PS2_SaveDeviceName(void);
void PS2_SaveDeviceToggle(void);

// ------------------------------------------------------------------------------------------------
// config.cfg (host.c, cmd.c)
// ------------------------------------------------------------------------------------------------
//
// QuakeSpasm writes config.cfg into <gamedir> and reads it back from the search path. On a
// console the player's settings belong on the memory card instead, not the USB stick or hard
// disk the game is on; under the emulator the host file stays first (src/ps2/save/save_api.cpp).
// The card's copy is read and written under the memory card notice, as saves are.

// The stream Host_WriteConfiguration writes the binds and cvars to, and its close, which stores
// the text where it goes. Null if it can't be written, with the reason on the console.
FILE * PS2_ConfigOpenWrite(void);
void PS2_ConfigCloseWrite(FILE * f);

// config.cfg's text for "exec config.cfg", on the hunk as COM_LoadHunkFile would load it; null if
// there is none anywhere.
const char * PS2_ConfigLoadHunk(void);

// ------------------------------------------------------------------------------------------------
// Engine state and functions the backend uses
// ------------------------------------------------------------------------------------------------

// r_part.c's live particles, simulated by CL_RunParticles and drawn by the backend's
// R_DrawParticles. QuakeSpasm had this static to r_part.c.
extern particle_t * active_particles;

// gl_screen.c's "ready to draw", set by SCR_Init: before it, the memory card notice draws over
// the boot splash instead. screen.h doesn't declare it.
extern qboolean scr_initialized;

// menu.c's message box, built from the gfx/box_*.lmp pieces on the menu canvas, which the memory
// card notice is drawn in. 'x' and 'y' are its top-left corner, border included; 'width' and
// 'lines' its inside, in 8-pixel characters, the width drawn two at a time. menu.h doesn't
// declare it.
void M_DrawTextBox(int x, int y, int width, int lines);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // PS2_ENGINE_HOOKS_H

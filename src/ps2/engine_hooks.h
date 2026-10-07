/* ================================================================================================
 * File: engine_hooks.h
 * Brief: What QuakeSpasm's C needs from the backend that no QuakeSpasm header declares: the PS2
 *        stand-ins for the few OpenGL calls made outside the renderer (the status bar's alpha
 *        and scissor, the view blend, the screenshot readback), the PS2's additions to the
 *        default config, and the engine state the backend reads that QuakeSpasm keeps to one
 *        file. Every engine line that uses this is tagged [PS2_QUAKE].
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
void PS2_DrawPicAlpha(int x, int y, qpic_t * pic, float alpha);

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

// ------------------------------------------------------------------------------------------------
// Screenshots (gl_screen.c)
// ------------------------------------------------------------------------------------------------

// The last finished frame, read back from GS VRAM as 8-bit RGB with the top row first: glwidth *
// glheight * 3 bytes. SCR_ScreenShot_f's glReadPixels.
void PS2_ReadPixels(byte * rgb);

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
// Engine state the backend reads
// ------------------------------------------------------------------------------------------------

// r_part.c's live particles, simulated by CL_RunParticles and drawn by the backend's
// R_DrawParticles. QuakeSpasm had this static to r_part.c.
extern particle_t * active_particles;

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // PS2_ENGINE_HOOKS_H

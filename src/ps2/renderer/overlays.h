#pragma once
/* ================================================================================================
 * File: overlays.h
 * Brief: The backend's on-screen debug overlays: the FPS counter and the profile, memory, VRAM
 *        and draw statistics panels, each behind its own cvar.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::overlay {

// Registers the overlay cvars. Call once, during Host_Init.
void Init();

// Draws whichever overlays are on, over everything else in the frame, in screen pixels. Call
// last thing before the frame is submitted (GL_EndRendering).
void Draw();

} // namespace ps2::overlay

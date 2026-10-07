#pragma once
/* ================================================================================================
 * File: view.h
 * Brief: The 3D view: QuakeSpasm's R_RenderView and what it draws - the world's visible surfaces
 *        through their texture chains, the brush models, the water - on the VU1 path. The Quake 1
 *        counterpart of the Quake II port's view.cpp; QuakeSpasm's gl_rmain.c, r_world.c and
 *        r_brush.c, with GL replaced by rs:: draws.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::view {

// Registers the view's cvars. R_Init calls it.
void Init();

// Reads the map's worldspawn for the view (wateralpha and its kin). R_NewMap calls it, once the
// brush models' draw data is built.
void NewMap();

// Draws the 3D view r_refdef describes: R_RenderView. Inside the frame, ahead of the 2D.
void RenderView();

// The sky's flat colour, as QuakeSpasm averages it for r_fastsky. Sky_LoadTexture sets it.
void SetSkyFlatColor(u8 r, u8 g, u8 b);

} // namespace ps2::view

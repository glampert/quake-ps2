#pragma once
/* ================================================================================================
 * File: view.h
 * Brief: The 3D view: QuakeSpasm's R_RenderView and what it draws - the world's visible surfaces
 *        through their texture chains, the entities, the water, the particles - on the VU1 path.
 *        The Quake 1 counterpart of the Quake II port's view.cpp; QuakeSpasm's gl_rmain.c,
 *        r_world.c and r_brush.c, with GL replaced by rs:: draws.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"

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

// ------------------------------------------------------------------------------------------------
// What the entity drawers share with the view, valid while RenderView runs
// ------------------------------------------------------------------------------------------------

// The frame's view-projection: world space to clip space (R_SetupGL's).
const math::Mat4 & ViewProjection();

// True when the entity's model, where it stands, is wholly outside the view: R_CullModelForEntity,
// with the bounds gl_model.c worked out for a model that yaws, pitches or rolls.
bool CullModelForEntity(const entity_t & e);

// An entity's model to world transform: R_RotateForEntity - yaw about Z, then -pitch about Y, then
// roll about X, scaled. Brush models hand it their pitch negated (R_DrawBrushModel's "stupid quake
// bug"); alias models don't.
math::Mat4 EntityMatrix(const vec3_t origin, const vec3_t angles, u8 scale);

// QuakeSpasm's cheat-safe draw modes this frame (R_SetupView), which take in single player only:
// r_fullbright, or a map without light data, draws everything unlit; r_lightmap draws the light
// alone, the textures white.
bool FullbrightMode();
bool LightmapMode();

} // namespace ps2::view

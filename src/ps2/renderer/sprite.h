#pragma once
/* ================================================================================================
 * File: sprite.h
 * Brief: Sprite models (.spr): the explosions, the bubbles, the light globes. QuakeSpasm's
 *        r_sprite.c on the VU1 path.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

namespace ps2::rs { class TriangleStream; }

namespace ps2::sprite {

// Draws one sprite entity (R_DrawSpriteModel): the frame the entity's frame and the time pick, as
// a quad that faces the camera or keeps its own orientation, as the sprite's type says. Gathers
// into 'stream', which the caller submits.
void DrawSpriteModel(rs::TriangleStream & stream, const entity_t & e);

} // namespace ps2::sprite

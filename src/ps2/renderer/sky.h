#pragma once
/* ================================================================================================
 * File: sky.h
 * Brief: The sky: QuakeSpasm's gl_sky.c on the VU1 path - its two scrolling cloud layers, or with
 *        r_fastsky a flat colour, behind the sky surfaces in view.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

namespace ps2::rs { class TriangleStream; }

namespace ps2::sky {

// Registers the sky's cvars and the "sky" command (Sky_Init). R_Init calls it.
void Init();

// Reads the map's worldspawn for a skybox name (Sky_NewMap). R_NewMap calls it.
void NewMap();

// Draws the sky behind the frame's visible sky surfaces - the world's and the brush entities' -
// and then writes those surfaces' depth, so whatever stands behind them stays hidden. Call first
// in the frame, ahead of the world, once its surfaces are marked: the clouds go down before
// anything else, on a box around the camera, and everything after draws over them.
void Draw(rs::TriangleStream & stream);

} // namespace ps2::sky

#pragma once
/* ================================================================================================
 * File: texmgr.h
 * Brief: The backend's side of the texture manager (texmgr.cpp), whose engine side is
 *        QuakeSpasm's TexMgr_* seam (gl_texmgr.h).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::tex {

// Loads gfx/palette.lmp into d_8to24table. VID_Init calls it, ahead of TexMgr_Init, because
// the GS builds its CLUTs from the palette as it comes up. Those never change afterwards, so
// neither does the palette: it is loaded once per run.
void LoadPalette();

} // namespace ps2::tex

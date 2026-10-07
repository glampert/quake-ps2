#pragma once
/* ================================================================================================
 * File: texmgr.h
 * Brief: The backend's side of the texture manager (texmgr.cpp), whose engine side is
 *        QuakeSpasm's TexMgr_* seam (gl_texmgr.h).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

struct gltexture_s;

namespace ps2::tex {

struct Texture;

// Loads gfx/palette.lmp into d_8to24table. VID_Init calls it, ahead of TexMgr_Init, because
// the GS builds its CLUTs from the palette as it comes up. Those never change afterwards, so
// neither does the palette: it is loaded once per run.
void LoadPalette();

// The PS2 texture behind one of the engine's, to bind for its draws. Null for a null gltexture_t
// and for one there is nothing to draw for (a warp image, a dummy without pixels, or an image the
// manager doesn't make textures for yet) - the caller draws a stand-in, or nothing.
const Texture * TextureFor(const gltexture_s * gl);

} // namespace ps2::tex

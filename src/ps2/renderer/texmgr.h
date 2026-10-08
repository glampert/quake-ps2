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
// and for one there is nothing to draw for (a warp image, a dummy without pixels) - the caller
// draws a stand-in, or nothing.
const Texture * TextureFor(const gltexture_s * gl);

// The palette translation that puts a player's shirt and pants colours (0 to 13) into a skin:
// QuakeSpasm's TexMgr_ReloadImage, where a colour row in the palette's upper half runs backwards.
void BuildPlayerTranslation(int shirt, int pants, unsigned char (&translation)[256]);

// Releases the textures the engine freed while the last frame was being recorded, which that frame
// may have been uploading. GL_BeginRendering calls it once rs::BeginFrame has waited that frame out.
void ReleaseRetiredTextures();

} // namespace ps2::tex

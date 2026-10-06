#pragma once
/* ================================================================================================
 * File: draw.h
 * Brief: The backend's side of the 2D module (draw.cpp), whose engine side is QuakeSpasm's
 *        draw.h seam (Draw_*, GL_SetCanvas).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::tex { struct Texture; }

namespace ps2::draw {

// The console font, conchars: a 16x16 grid of 8x8 glyphs, with the font's background cut out.
// For the backend's own text (the debug overlays), drawn straight in screen pixels with
// rs::DrawTexturedRect. Valid from Draw_Init on.
const tex::Texture & Conchars();

} // namespace ps2::draw

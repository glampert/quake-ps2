#pragma once
/* ================================================================================================
 * File: lightmap.h
 * Brief: The lightmaps: the atlases the BSP's baked light samples are packed into, and the
 *        rebuilds that fold in animated light styles and dynamic lights. QuakeSpasm's r_brush.c
 *        lightmap half (GL_CreateSurfaceLightmap, R_BuildLightMap, R_RenderDynamicLightmaps), on
 *        the GS.
 *
 *  Each atlas holds one byte per luxel: its light, 0 to 255 where 128 is the texture's own
 *  colour, sampled through the light-ramp CLUT (tex::PixelFormat::Light8) by a pass that
 *  multiplies the framebuffer by it - QuakeSpasm's gl_overbright. Intensity only: the GS blend
 *  multiplies by an alpha, never a second colour, and Quake's light is white anyway (see
 *  gl_model.c's Mod_LoadLighting).
 *
 *  Map load: brush.cpp calls CreateSurfaceLightmap for every lit surface of every brush model,
 *  between BeginBuilding and EndBuilding, before it bakes the surfaces' vertices - which carry
 *  where the surface's block landed. Frame time: the view calls UpdateSurface for every visible
 *  lit surface, which rebuilds the block if its lighting moved and has the atlas re-uploaded.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include <tamtypes.h>

namespace ps2::tex { struct Texture; }

namespace ps2::lm {

// Atlas dimensions, in luxels: QuakeSpasm's LMBLOCK_WIDTH and LMBLOCK_HEIGHT. A surface's
// lightmap UVs are normalized against them.
constexpr int kAtlasWidth  = 256;
constexpr int kAtlasHeight = 256;

// World units one luxel covers, fixed by the BSP format: a surface's luxel count is
// (extents >> 4) + 1.
constexpr int kLuxelSizeUnits = 16;

// Atlases a map may use. Allocated on demand, so the headroom costs nothing until a map reaches
// for it; running out is a Sys_Error telling you to raise it. Each is 64 KB of EE RAM, and of GS
// VRAM while it is resident, which is the budget to watch: the texture heap is about 1.3 MB.
constexpr int kMaxAtlases = 16;

// Releases the previous map's atlases and starts packing the first of the next.
void BeginBuilding();

// Packs the surface's luxels into an atlas and bakes its lighting there, filling in
// surf.lightmaptexturenum, light_s and light_t. Only for surfaces that have a lightmap - not
// SURF_DRAWTILED ones (sky, unlit water, missing textures).
void CreateSurfaceLightmap(msurface_t & surf);

// Closes the atlas being filled. Nothing may be packed until the next BeginBuilding.
void EndBuilding();

// Frees the atlases without starting a new build.
void ReleaseAtlases();

// Rebuilds the surface's block if its lighting changed since it was baked - a light style that
// moved, or a dynamic light touching it this frame or the last - and marks its atlas for
// re-upload. QuakeSpasm's R_RenderDynamicLightmaps. Call once per visible lit surface per frame,
// before the lightmap pass, after R_AnimateLight and R_PushDlights have run.
void UpdateSurface(msurface_t & surf);

// Atlases in use by the current map, and the texture to bind for one. Indices are the surfaces'
// lightmaptexturenum.
int NumAtlases();
const tex::Texture & AtlasTexture(int index);

} // namespace ps2::lm

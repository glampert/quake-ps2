#pragma once
/* ================================================================================================
 * File: brush.h
 * Brief: The brush models' draw data: every surface's vertices, baked once a map has loaded in
 *        the form the VU1 path takes them, so a frame only copies them into the command buffer.
 *        QuakeSpasm's GL_BuildLightmaps / BuildSurfaceDisplayList, on the PS2.
 *
 *  The renderer reads QuakeSpasm's own BSP structures for everything else - nodes, leafs, surface
 *  flags, texinfo, lightmap placement - and keeps here only what the GL renderer kept in its
 *  glpoly_t chains and vertex buffer. Each brush model the map uses gets a ModelDraw, which its
 *  qmodel_t::ps2_render points at; the world's inline submodels ("*1", "*2", ...) share the
 *  world's, as they share its surfaces.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"

#include <tamtypes.h>

namespace ps2::brush {

// How a surface's vertices are laid out.
enum class Geometry : u8
{
    None,      // Nothing to draw (a surface with too few edges).
    Fan,       // One convex polygon, numVerts corners, drawn as a fan from the first. Every
               // surface but the unlit liquids.
    Triangles, // A triangle list: an unlit liquid's surface, cut on a 32-unit grid so the VU1
               // warp has vertices close enough together to bend it by.
};

// One surface's vertices.
//
// Positions are in the model's space. The diffuse UVs are normalized to the texture, except on a
// turbulent surface, where they stay in raw texels for the VU1 warp; the lightmap UVs, in the two
// lanes DrawVertex keeps spare, are normalized to the atlas. The colour is the GS modulate
// identity, which leaves the texel as it is.
struct SurfaceDraw
{
    vu1::DrawVertex * verts;
    u16               numVerts;
    Geometry          geometry;

    // Per frame: the next visible surface on this one's lightmap atlas, threaded by the view's
    // world pass for the lightmap pass that follows.
    msurface_t * lightmapChain;
};

// A brush model's draw data. 'surfaces' runs parallel to qmodel_t::surfaces.
struct ModelDraw
{
    vu1::DrawVertex * verts;
    int               numVerts;
    SurfaceDraw *     surfaces;
    int               numSurfaces;
};

// Builds the lightmaps and the draw data of every brush model the map precached
// (cl.model_precache), freeing the previous map's. R_NewMap calls it.
void BuildForNewMap();

// The draw data of a brush model of the current map.
Q_ALWAYS_INLINE const ModelDraw & DrawFor(const qmodel_t & model)
{
    PS2_AssertMsg(model.ps2_render != nullptr, "Brush model without draw data!");
    return *static_cast<const ModelDraw *>(model.ps2_render);
}

// The draw data of one of the model's surfaces. Mutable for the lightmap chain.
Q_ALWAYS_INLINE SurfaceDraw & DrawFor(const qmodel_t & model, const msurface_t & surf)
{
    const ModelDraw & draw = DrawFor(model);
    const int index = static_cast<int>(&surf - model.surfaces);
    PS2_Assert(index >= 0 && index < draw.numSurfaces);
    return draw.surfaces[index];
}

} // namespace ps2::brush

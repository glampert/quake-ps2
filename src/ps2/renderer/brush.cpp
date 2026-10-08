/* ================================================================================================
 * File: brush.cpp
 * Brief: The brush models' draw data, built when a map has loaded. See brush.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/brush.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/render_system.h"

#include <cmath>
#include <cstddef>
#include <cstring>

// gl_model.c hands BSP textures to the texture manager straight off the hunk, where the pixels
// follow their texture_t: padded to a multiple of 16 bytes, so the GS can upload them in place.
static_assert((sizeof(texture_t) % 16) == 0, "texture_t must keep the pixels after it 16-byte aligned");

// zone.c's Cache_Free finds a model from its cache_user_t by assuming the field is the last in
// qmodel_t; qmodel_t::ps2_render had to go in ahead of it.
static_assert(offsetof(qmodel_t, cache) + sizeof(cache_user_t) == sizeof(qmodel_t),
              "qmodel_t::cache must stay the last field");

namespace ps2::brush {
namespace {

// The vertex colour every surface bakes: the GS modulate identity, which leaves the texel as it is,
// at full alpha. The passes that want another colour put it on as they copy.
constexpr u32 kModulateIdentity = vu1::PackColorRGBA(128, 128, 128, 0x80);

// The brush models with draw data of their own - the world and the separately loaded .bsp models
// (the ammo and health boxes) - so the next map can free them. A map precaches a handful.
constexpr int kMaxModelDraws = 64;
static ModelDraw * s_modelDraws[kMaxModelDraws];
static u32         s_modelDrawBytes[kMaxModelDraws];
static int         s_numModelDraws = 0;

// A surface's vertex positions as the BSP has them: a convex polygon, one corner per edge.
Q_ALWAYS_INLINE const float * SurfaceVertex(const qmodel_t & model, const msurface_t & surf, const int i)
{
    const int lindex = model.surfedges[surf.firstedge + i];
    return (lindex > 0) ? model.vertexes[model.edges[lindex].v[0]].position
                        : model.vertexes[model.edges[-lindex].v[1]].position;
}

// ------------------------------------------------------------------------------------------------
// Turbulent surfaces
// ------------------------------------------------------------------------------------------------

// The water's ripple repeats every 128 texels (vu1::kTurbTurnsPerTexel), and VU1 bends it at the
// vertices, so a turbulent surface is cut into cells a quarter of that across - four vertices a
// period, whose straight segments follow the ripple closely enough. QuakeSpasm cuts its water too
// (gl_warp.c's GL_SubdivideSurface, at gl_subdivide_size, 128 by default), for a warp that sampled
// every texel; this cut replaces that one.
constexpr float kWarpCellSize = 32.0f;

// A grid line closer than this to a piece's edge doesn't cut it, so no sliver comes off: the 8
// units gl_warp.c's SubdividePolygon keeps.
constexpr float kWarpCutMargin = 8.0f;

// Corners a turbulent surface may have (GL_SubdivideSurface's 64), and a piece of one mid-cut: a
// cut adds at most one corner to a convex piece.
constexpr int kMaxWarpSurfaceCorners = 64;
constexpr int kMaxWarpCorners        = kMaxWarpSurfaceCorners + 8;

// A convex piece of a turbulent surface.
struct WarpPiece
{
    int   numCorners;
    float corners[kMaxWarpCorners][3];
};

void AppendCorner(WarpPiece & piece, const float * const corner)
{
    if (piece.numCorners >= kMaxWarpCorners)
    {
        Sys_Error("Turbulent surface cut into more than %d corners", kMaxWarpCorners);
    }
    float * const out = piece.corners[piece.numCorners++];
    out[0] = corner[0];
    out[1] = corner[1];
    out[2] = corner[2];
}

// Splits a piece at the plane where a corner's 'axis' coordinate is 'at'.
void SplitWarpPiece(const WarpPiece & in, const int axis, const float at, WarpPiece & below, WarpPiece & above)
{
    below.numCorners = 0;
    above.numCorners = 0;

    for (int i = 0; i < in.numCorners; ++i)
    {
        const float * const v    = in.corners[i];
        const float * const next = in.corners[(i + 1) % in.numCorners];
        const float d     = v[axis] - at;
        const float dNext = next[axis] - at;

        if (d <= 0.0f)
        {
            AppendCorner(below, v);
        }
        if (d >= 0.0f)
        {
            AppendCorner(above, v);
        }

        if ((d < 0.0f && dNext > 0.0f) || (d > 0.0f && dNext < 0.0f))
        {
            const float frac = d / (d - dNext);
            float point[3];
            for (int k = 0; k < 3; ++k)
            {
                point[k] = v[k] + (frac * (next[k] - v[k]));
            }
            point[axis] = at;

            AppendCorner(below, point);
            AppendCorner(above, point);
        }
    }
}

// Cuts a piece at every grid line across 'axis', then each strip that leaves across the axes after
// it, and hands each cell to 'emit'. Iterative along an axis, so how deep this goes is the three
// axes, whatever the size of the surface.
template<typename Emit>
void CutWarpPiece(const WarpPiece & piece, const int axis, Emit & emit)
{
    if (axis == 3)
    {
        emit(piece);
        return;
    }

    float lo = piece.corners[0][axis];
    float hi = lo;
    for (int i = 1; i < piece.numCorners; ++i)
    {
        lo = (piece.corners[i][axis] < lo) ? piece.corners[i][axis] : lo;
        hi = (piece.corners[i][axis] > hi) ? piece.corners[i][axis] : hi;
    }

    WarpPiece remainder = piece;
    for (float at = (std::floor(lo / kWarpCellSize) + 1.0f) * kWarpCellSize; at < hi - kWarpCutMargin; at += kWarpCellSize)
    {
        if (at - lo < kWarpCutMargin)
        {
            continue;
        }

        WarpPiece below, above;
        SplitWarpPiece(remainder, axis, at, below, above);
        if (below.numCorners >= 3)
        {
            CutWarpPiece(below, axis + 1, emit);
        }
        remainder = above;
        lo = at;
    }

    if (remainder.numCorners >= 3)
    {
        CutWarpPiece(remainder, axis + 1, emit);
    }
}

// Cuts a turbulent surface's polygon into its cells and hands each to 'emit'.
template<typename Emit>
void CutTurbulentSurface(const qmodel_t & model, const msurface_t & surf, Emit & emit)
{
    if (surf.numedges > kMaxWarpSurfaceCorners)
    {
        Sys_Error("Turbulent surface with %d corners (%d at most)", surf.numedges, kMaxWarpSurfaceCorners);
    }

    WarpPiece whole;
    whole.numCorners = 0;
    for (int i = 0; i < surf.numedges; ++i)
    {
        AppendCorner(whole, SurfaceVertex(model, surf, i));
    }
    CutWarpPiece(whole, 0, emit);
}

// The vertices of a turbulent surface's cells, each fanned into triangles, with raw texel
// coordinates as gl_warp.c's SubdividePolygon gave them - the texture's axes without its offset, as
// QuakeSpasm maps its water: the VU1 warp bends them and divides by the texture's size itself.
// Unlit water has no lightmap UVs to carry.
void BakeTurbulentSurface(const qmodel_t & model, const msurface_t & surf, vu1::DrawVertex * out)
{
    const mtexinfo_t & texinfo = *surf.texinfo;

    auto emit = [&out, &texinfo](const WarpPiece & cell)
    {
        for (int t = 1; t < cell.numCorners - 1; ++t)
        {
            const int corners[3] = { 0, t, t + 1 };
            for (const int c : corners)
            {
                const float * const v = cell.corners[c];
                out->position   = { v[0], v[1], v[2] };
                out->rgba       = kModulateIdentity;
                out->s          = DotProduct(v, texinfo.vecs[0]);
                out->t          = DotProduct(v, texinfo.vecs[1]);
                out->lightmap_s = 0.0f;
                out->lightmap_t = 0.0f;
                ++out;
            }
        }
    };
    CutTurbulentSurface(model, surf, emit);
}

// ------------------------------------------------------------------------------------------------
// Surfaces
// ------------------------------------------------------------------------------------------------

// Vertices a surface bakes into, and how they are laid out.
SurfaceDraw MeasureSurface(const qmodel_t & model, const msurface_t & surf)
{
    SurfaceDraw draw = {};

    if (surf.numedges < 3)
    {
        return draw;
    }

    // The unlit liquids (lit water, from a map with its own light for it, draws as a wall).
    if ((surf.flags & SURF_DRAWTURB) != 0 && (surf.flags & SURF_DRAWTILED) != 0)
    {
        int verts = 0;
        auto count = [&verts](const WarpPiece & cell) { verts += 3 * (cell.numCorners - 2); };
        CutTurbulentSurface(model, surf, count);

        if (verts > 0xFFFF)
        {
            Sys_Error("Turbulent surface cut into %d vertices", verts);
        }
        draw.numVerts = static_cast<u16>(verts);
        draw.geometry = (verts > 0) ? Geometry::Triangles : Geometry::None;
    }
    else
    {
        draw.numVerts = static_cast<u16>(surf.numedges);
        draw.geometry = Geometry::Fan;
    }
    return draw;
}

// A surface's convex polygon, with QuakeSpasm's BuildSurfaceDisplayList texture coordinates: the
// diffuse ones normalized by the texture's size on disk, the lightmap ones placed in the surface's
// atlas block, at luxel centres.
void BakeSurfacePolygon(const qmodel_t & model, const msurface_t & surf, vu1::DrawVertex * out)
{
    const mtexinfo_t & texinfo = *surf.texinfo;
    const float sdiv = static_cast<float>(texinfo.texture->width);
    const float tdiv = static_cast<float>(texinfo.texture->height);

    const bool lit = (surf.flags & SURF_DRAWTILED) == 0;
    constexpr float kAtlasS = 1.0f / static_cast<float>(lm::kAtlasWidth  * lm::kLuxelSizeUnits);
    constexpr float kAtlasT = 1.0f / static_cast<float>(lm::kAtlasHeight * lm::kLuxelSizeUnits);

    for (int i = 0; i < surf.numedges; ++i, ++out)
    {
        const float * const vec = SurfaceVertex(model, surf, i);

        const float s = DotProduct(vec, texinfo.vecs[0]) + texinfo.vecs[0][3];
        const float t = DotProduct(vec, texinfo.vecs[1]) + texinfo.vecs[1][3];

        out->position = { vec[0], vec[1], vec[2] };
        out->rgba     = kModulateIdentity;
        out->s        = s / sdiv;
        out->t        = t / tdiv;

        if (lit)
        {
            const float lightS = s - static_cast<float>(surf.texturemins[0]) + static_cast<float>((surf.light_s * 16) + 8);
            const float lightT = t - static_cast<float>(surf.texturemins[1]) + static_cast<float>((surf.light_t * 16) + 8);
            out->lightmap_s = lightS * kAtlasS;
            out->lightmap_t = lightT * kAtlasT;
        }
        else
        {
            out->lightmap_s = 0.0f;
            out->lightmap_t = 0.0f;
        }
    }
}

// Builds a brush model's draw data in one heap block: the ModelDraw, its surfaces, then its
// vertices, qword aligned for the copies into the command buffer.
ModelDraw * BuildModelDraw(qmodel_t & model)
{
    int numVerts = 0;
    for (int i = 0; i < model.numsurfaces; ++i)
    {
        numVerts += MeasureSurface(model, model.surfaces[i]).numVerts;
    }

    const u32 headerBytes  = (sizeof(ModelDraw) + 15u) & ~15u;
    const u32 surfaceBytes = ((static_cast<u32>(model.numsurfaces) * sizeof(SurfaceDraw)) + 15u) & ~15u;
    const u32 vertexBytes  = static_cast<u32>(numVerts) * sizeof(vu1::DrawVertex);
    const u32 totalBytes   = headerBytes + surfaceBytes + vertexBytes;

    PS2_AssertMsg(s_numModelDraws < kMaxModelDraws, "Out of brush model draw slots!");
    byte * const block = static_cast<byte *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(16), totalBytes, ps2::heap::MemTag::WorldMdl));
    s_modelDraws[s_numModelDraws]     = static_cast<ModelDraw *>(static_cast<void *>(block));
    s_modelDrawBytes[s_numModelDraws] = totalBytes;

    ModelDraw & draw  = *s_modelDraws[s_numModelDraws++];
    draw.surfaces     = static_cast<SurfaceDraw *>(static_cast<void *>(block + headerBytes));
    draw.verts        = static_cast<vu1::DrawVertex *>(static_cast<void *>(block + headerBytes + surfaceBytes));
    draw.numVerts     = numVerts;
    draw.numSurfaces  = model.numsurfaces;

    vu1::DrawVertex * cursor = draw.verts;
    for (int i = 0; i < model.numsurfaces; ++i)
    {
        const msurface_t & surf = model.surfaces[i];
        SurfaceDraw & surfDraw = draw.surfaces[i];

        surfDraw = MeasureSurface(model, surf);
        surfDraw.verts = cursor;

        if (surfDraw.geometry == Geometry::Triangles)
        {
            BakeTurbulentSurface(model, surf, cursor);
        }
        else if (surfDraw.geometry == Geometry::Fan)
        {
            BakeSurfacePolygon(model, surf, cursor);
        }
        cursor += surfDraw.numVerts;
    }

    return &draw;
}

void FreeModelDraws()
{
    for (int i = 0; i < s_numModelDraws; ++i)
    {
        ps2::heap::Free(s_modelDraws[i], s_modelDrawBytes[i], ps2::heap::MemTag::WorldMdl);
        s_modelDraws[i] = nullptr;
    }
    s_numModelDraws = 0;
}

} // namespace

void BuildForNewMap()
{
    // The atlases about to be freed may still be uploading for the frame the GS is drawing.
    rs::FinishFrameInFlight();
    FreeModelDraws();

    // QuakeSpasm's GL_BuildLightmaps: every lit surface of every brush model the map precached gets
    // its atlas block, then its vertices, which carry where the block landed. The world's inline
    // submodels are skipped, as there: their surfaces are the world's.
    lm::BeginBuilding();
    for (int j = 1; j < MAX_MODELS && cl.model_precache[j] != nullptr; ++j)
    {
        qmodel_t & model = *cl.model_precache[j];
        if (model.type != mod_brush || model.name[0] == '*')
        {
            continue;
        }
        for (int i = 0; i < model.numsurfaces; ++i)
        {
            if ((model.surfaces[i].flags & SURF_DRAWTILED) == 0)
            {
                lm::CreateSurfaceLightmap(model.surfaces[i]);
            }
        }
    }
    lm::EndBuilding();

    int numVerts = 0;
    for (int j = 1; j < MAX_MODELS && cl.model_precache[j] != nullptr; ++j)
    {
        qmodel_t & model = *cl.model_precache[j];
        if (model.type == mod_brush && model.name[0] != '*')
        {
            ModelDraw * const draw = BuildModelDraw(model);
            model.ps2_render = draw;
            numVerts += draw->numVerts;
        }
    }

    // The submodels after, now the world has its data to share.
    for (int j = 1; j < MAX_MODELS && cl.model_precache[j] != nullptr; ++j)
    {
        qmodel_t & model = *cl.model_precache[j];
        if (model.type == mod_brush && model.name[0] == '*')
        {
            model.ps2_render = cl.worldmodel->ps2_render;
        }
    }

    Con_DPrintf("Brush models: %d, %d vertices, %d KB.\n", s_numModelDraws, numVerts,
                (numVerts * static_cast<int>(sizeof(vu1::DrawVertex))) / 1024);
}

} // namespace ps2::brush

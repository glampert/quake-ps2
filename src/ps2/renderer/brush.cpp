/* ================================================================================================
 * File: brush.cpp
 * Brief: The brush models' draw data, built when a map has loaded. See brush.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/brush.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/render_system.h"

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

// Vertices a surface bakes into, and how they are laid out.
SurfaceDraw MeasureSurface(const msurface_t & surf)
{
    SurfaceDraw draw = {};

    if ((surf.flags & SURF_DRAWTURB) != 0 && surf.polys != nullptr)
    {
        // gl_warp.c's subdivided polygons follow the whole one in the chain, each a convex fan.
        int verts = 0;
        for (const glpoly_t * poly = surf.polys->next; poly != nullptr; poly = poly->next)
        {
            verts += 3 * (poly->numverts - 2);
        }
        draw.numVerts = static_cast<u16>(verts);
        draw.geometry = (verts > 0) ? Geometry::Triangles : Geometry::None;
    }
    else if (surf.numedges >= 3)
    {
        draw.numVerts = static_cast<u16>(surf.numedges);
        draw.geometry = Geometry::Fan;
    }
    return draw;
}

// The vertices of a turbulent surface's subdivided polygons, fanned into a triangle list, with
// the raw texel coordinates gl_warp.c's SubdividePolygon gave them: the VU1 warp bends those and
// divides by the texture's size itself. Unlit water has no lightmap UVs to carry.
void BakeTurbulentSurface(const msurface_t & surf, vu1::DrawVertex * out)
{
    for (const glpoly_t * poly = surf.polys->next; poly != nullptr; poly = poly->next)
    {
        for (int t = 1; t < poly->numverts - 1; ++t)
        {
            const int corners[3] = { 0, t, t + 1 };
            for (const int c : corners)
            {
                const float * const v = poly->verts[c];
                out->position   = { v[0], v[1], v[2] };
                out->rgba       = kModulateIdentity;
                out->s          = v[3];
                out->t          = v[4];
                out->lightmap_s = 0.0f;
                out->lightmap_t = 0.0f;
                ++out;
            }
        }
    }
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
        numVerts += MeasureSurface(model.surfaces[i]).numVerts;
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

        surfDraw = MeasureSurface(surf);
        surfDraw.verts = cursor;

        if (surfDraw.geometry == Geometry::Triangles)
        {
            BakeTurbulentSurface(surf, cursor);
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

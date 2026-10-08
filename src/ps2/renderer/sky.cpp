/* ================================================================================================
 * File: sky.cpp
 * Brief: The sky: QuakeSpasm's gl_sky.c on the VU1 path. See sky.h.
 *
 *  A sky texture is two layers, a solid back one and a front one with holes, each scrolling at its
 *  own speed, and what shows through a sky surface is decided by direction alone: the layers sit
 *  on a flattened sphere around the camera. QuakeSpasm draws them on a box around the camera, each
 *  face tessellated finely enough for its per-vertex texture coordinates to follow the sphere, and
 *  only over the parts of it the sky surfaces in view cover (their bounds on each face, found by
 *  cutting each surface along the planes between the faces).
 *
 *  It draws the surfaces first, writing their depth, then the box behind them with a depth test
 *  that passes only where a sky surface is. The GS can't test for "farther": its z-test passes
 *  nearer or equal, or greater. So the order turns round here - the box goes down first, before
 *  anything else in the frame and with no depth of its own, over the bounds alone; then the sky
 *  surfaces write their depth and nothing else; then the world draws over the box wherever it is
 *  in front, and is hidden wherever a sky surface is in front of it. The one difference: a gap in
 *  a map that shows the void would show sky here, where QuakeSpasm shows the clear colour.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/sky.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/brush.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"

#include <cmath>
#include <cstring>

extern "C" {
// The view's.
extern cvar_t r_drawworld, r_drawentities;
} // extern "C"

namespace ps2::sky {
namespace {

// ------------------------------------------------------------------------------------------------
// Cvars and constants
// ------------------------------------------------------------------------------------------------

// QuakeSpasm's: the flat colour in place of the layers; how finely a box face is tessellated (cells
// across a top or bottom face, twice that up a side); the front layer's opacity.
static cvar_t s_fastSky    = ps2::MakeCvar("r_fastsky",     "0",  CVAR_NONE);
static cvar_t s_skyQuality = ps2::MakeCvar("r_sky_quality", "12", CVAR_NONE);
static cvar_t s_skyAlpha   = ps2::MakeCvar("r_skyalpha",    "1",  CVAR_NONE);

// r_sky_quality's ceiling here, which sizes the face grid below. QuakeSpasm has none.
constexpr int kMaxSkyQuality = 16;

// Half the size of the box the layers are drawn on, in world units. Any size would do - the box is
// drawn first, with no depth, and only directions matter to the layers - while it stays clear of
// the near plane and inside the far one.
constexpr float kBoxSize = 1024.0f;

// The six faces of the box.
constexpr int kNumFaces = 6;

// gl_sky.c's ON_EPSILON, the plane-side slack of the cut between faces.
constexpr float kOnPlaneEpsilon = 0.1f;

// Room for one sky polygon mid-cut: a convex polygon gains at most a corner per plane, and the cut
// writes a copy of the first corner past the last.
constexpr int kMaxSkyClipVerts = 72;
constexpr int kSkyClipStages   = 6;

// The GS modulate identity at full alpha.
constexpr u32 kModulateIdentity = vu1::PackColorRGBA(128, 128, 128, 0x80);

// ------------------------------------------------------------------------------------------------
// gl_sky.c's tables, kept verbatim
// ------------------------------------------------------------------------------------------------

// The planes through the camera that separate the box's faces. Unnormalized on purpose: only the
// sign of the dot product and the ratio of two distances matter.
static const vec3_t s_skyClip[kNumFaces] = {
    {  1.0f,  1.0f, 0.0f },
    {  1.0f, -1.0f, 0.0f },
    {  0.0f, -1.0f, 1.0f },
    {  0.0f,  1.0f, 1.0f },
    {  1.0f,  0.0f, 1.0f },
    { -1.0f,  0.0f, 1.0f }
};

// Face-local (s, t, distance) to a world direction, and back. An entry k means component |k|-1,
// negated when k is negative, where 1, 2 and 3 stand for s, t and the face's own axis.
static const int s_stToVec[kNumFaces][3] = {
    {  3, -1,  2 },
    { -3,  1,  2 },
    {  1,  3,  2 },
    { -1, -3,  2 },
    { -2, -1,  3 }, // straight up
    {  2, -1, -3 }  // straight down
};

static const int s_vecToSt[kNumFaces][3] = {
    { -2,  3,  1 },
    {  2,  3, -1 },
    {  1,  3,  2 },
    { -1,  3, -2 },
    { -2, -1,  3 },
    { -2,  1, -3 }
};

// ------------------------------------------------------------------------------------------------
// Sky state
// ------------------------------------------------------------------------------------------------

// The map's layers: the back one through Quake's palette, the front one as RGBA, its holes
// transparent - GL's alpha blend, filtered, wants a colour in them, which an 8-bit image can only
// have as the palette's 255 (see FixAlphaEdges). Both in one block of pixels.
static const tex::Texture * s_solidLayer = nullptr;
static const tex::Texture * s_alphaLayer = nullptr;
static void *               s_layerPixels = nullptr;
static u32                  s_layerBytes  = 0;

// The flat colour r_fastsky draws: the front layer's opaque texels averaged.
static u32 s_flatColor = vu1::PackColorRGBA(64, 64, 96, 0x80);

// What the frame's sky surfaces cover of each box face, in face-local [-1, 1] coordinates. An empty
// interval (mins > maxs) means none of them reached it.
static float s_skyMins[2][kNumFaces];
static float s_skyMaxs[2][kNumFaces];

// ClipSkyPolygon's working buffers, by the stage that fills them: a stage's output is only read by
// the stage below it, and the first piece's whole subtree finishes before the second starts.
static vec3_t s_skyClipVerts[kSkyClipStages][2][kMaxSkyClipVerts];

// A box face's grid, filled per face per frame as the vertices the back layer draws with, then
// turned in place into the front layer's: the same but for a constant added to the texture
// coordinates, and the colour. Built once and copied into each triangle, a corner costs the copy.
static vu1::DrawVertex s_grid[(kMaxSkyQuality + 1) * ((2 * kMaxSkyQuality) + 1)];

// ------------------------------------------------------------------------------------------------
// Loading
// ------------------------------------------------------------------------------------------------

// Whether 'n' is a power of two, as a tiling layer must be (see tex::StScaleFor).
constexpr bool IsPowerOfTwo(const int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

void ClearLayers()
{
    if (s_solidLayer == nullptr && s_alphaLayer == nullptr)
    {
        return;
    }

    // The frame the GS may still be drawing can be uploading them.
    rs::FinishFrameInFlight();

    if (s_solidLayer != nullptr)
    {
        tex::Destroy(*s_solidLayer);
        s_solidLayer = nullptr;
    }
    if (s_alphaLayer != nullptr)
    {
        tex::Destroy(*s_alphaLayer);
        s_alphaLayer = nullptr;
    }
    heap::Free(s_layerPixels, s_layerBytes, heap::MemTag::TexImage);
    s_layerPixels = nullptr;
    s_layerBytes  = 0;
}

// QuakeSpasm's TexMgr_AlphaEdgeFix: each transparent texel takes the average colour of its opaque
// neighbours, wrapping round the edges as the layer tiles, so filtering blends a hole's edge
// towards the cloud rather than towards whatever colour the hole had.
void FixAlphaEdges(u32 * const texels, const int width, const int height)
{
    for (int y = 0; y < height; ++y)
    {
        const int rows[3] = { (y + height - 1) % height, y, (y + 1) % height };
        for (int x = 0; x < width; ++x)
        {
            u32 & texel = texels[(y * width) + x];
            if ((texel >> 24) != 0)
            {
                continue;
            }

            const int cols[3] = { (x + width - 1) % width, x, (x + 1) % width };
            u32 r = 0, g = 0, b = 0, n = 0;
            for (const int row : rows)
            {
                for (const int col : cols)
                {
                    const u32 neighbour = texels[(row * width) + col];
                    if ((neighbour >> 24) != 0)
                    {
                        r += neighbour & 0xFFu;
                        g += (neighbour >> 8) & 0xFFu;
                        b += (neighbour >> 16) & 0xFFu;
                        ++n;
                    }
                }
            }
            if (n != 0)
            {
                texel = (r / n) | ((g / n) << 8) | ((b / n) << 16); // alpha stays 0
            }
        }
    }
}

// Makes the two layer textures over one block, each width x height: the back one as the palette
// indices 'backIndex(x, y)' gives, the front one as the RGBA texels 'frontTexel(x, y)' does, whose
// average opaque colour becomes the flat sky's. Read straight out of the texture, which is in the
// BSP file's buffer: the hunk's temporary space, which anything taken from it now would free.
template<typename BackIndex, typename FrontTexel>
void CreateLayers(const qmodel_t & model, const texture_t & mt, const int width, const int height,
                  BackIndex backIndex, FrontTexel frontTexel)
{
    const int count = width * height;
    s_layerBytes  = static_cast<u32>(count * 5);
    s_layerPixels = heap::AllocAligned(heap::MemAlign(16), s_layerBytes, heap::MemTag::TexImage);

    byte * const backPixels  = static_cast<byte *>(s_layerPixels);
    u32 *  const frontPixels = static_cast<u32 *>(static_cast<void *>(backPixels + count));

    u32 r = 0, g = 0, b = 0, opaque = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int i = (y * width) + x;
            backPixels[i] = backIndex(x, y);

            const u32 texel = frontTexel(x, y);
            frontPixels[i] = texel;
            if ((texel >> 24) != 0)
            {
                r += texel & 0xFFu;
                g += (texel >> 8) & 0xFFu;
                b += (texel >> 16) & 0xFFu;
                ++opaque;
            }
        }
    }
    FixAlphaEdges(frontPixels, width, height);

    if (opaque != 0)
    {
        s_flatColor = vu1::PackColorRGBA(r / opaque, g / opaque, b / opaque, 0x80);
    }

    char name[MAX_QPATH];
    q_snprintf(name, sizeof(name), "%s:%s_back", model.name, mt.name); // truncated as QuakeSpasm truncates it
    s_solidLayer = &tex::Create(name, backPixels, width, height, tex::PixelFormat::Palette8,
                                tex::TexComponents::RGB, tex::ImageType::Sky);

    q_snprintf(name, sizeof(name), "%s:%s_front", model.name, mt.name);
    s_alphaLayer = &tex::Create(name, frontPixels, width, height, tex::PixelFormat::RGBA32,
                                tex::TexComponents::RGBA, tex::ImageType::Sky);
}

// ------------------------------------------------------------------------------------------------
// Bounds (gl_sky.c's Sky_ProjectPoly, Sky_ClipPoly)
// ------------------------------------------------------------------------------------------------

void ClearBounds()
{
    for (int i = 0; i < kNumFaces; ++i)
    {
        s_skyMins[0][i] = s_skyMins[1][i] =  9999.0f;
        s_skyMaxs[0][i] = s_skyMaxs[1][i] = -9999.0f;
    }
}

bool AnyBounds()
{
    for (int i = 0; i < kNumFaces; ++i)
    {
        if (s_skyMins[0][i] < s_skyMaxs[0][i] && s_skyMins[1][i] < s_skyMaxs[1][i])
        {
            return true;
        }
    }
    return false;
}

// Picks the face a fully cut, camera-relative polygon lands on and grows that face's bounds to it.
void ProjectSkyPolygon(const int nump, const vec3_t * vecs)
{
    // The polygon is on one face by now, so the sum of its corners points at it: the dominant
    // component picks the axis, its sign the side.
    vec3_t v = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < nump; ++i)
    {
        VectorAdd(vecs[i], v, v);
    }

    const float av[3] = { math::Fabsf(v[0]), math::Fabsf(v[1]), math::Fabsf(v[2]) };

    int axis;
    if (av[0] > av[1] && av[0] > av[2])
    {
        axis = (v[0] < 0.0f) ? 1 : 0;
    }
    else if (av[1] > av[2] && av[1] > av[0])
    {
        axis = (v[1] < 0.0f) ? 3 : 2;
    }
    else
    {
        axis = (v[2] < 0.0f) ? 5 : 4;
    }

    for (int i = 0; i < nump; ++i)
    {
        const float * const vec = vecs[i];

        int j = s_vecToSt[axis][2];
        const float dv = (j > 0) ? vec[j - 1] : -vec[-j - 1];
        if (dv < 0.001f)
        {
            continue; // on the camera, or behind the face: no projection
        }

        j = s_vecToSt[axis][0];
        const float s = ((j < 0) ? -vec[-j - 1] : vec[j - 1]) / dv;

        j = s_vecToSt[axis][1];
        const float t = ((j < 0) ? -vec[-j - 1] : vec[j - 1]) / dv;

        s_skyMins[0][axis] = (s < s_skyMins[0][axis]) ? s : s_skyMins[0][axis];
        s_skyMins[1][axis] = (t < s_skyMins[1][axis]) ? t : s_skyMins[1][axis];
        s_skyMaxs[0][axis] = (s > s_skyMaxs[0][axis]) ? s : s_skyMaxs[0][axis];
        s_skyMaxs[1][axis] = (t > s_skyMaxs[1][axis]) ? t : s_skyMaxs[1][axis];
    }
}

// Cuts a camera-relative polygon along the planes between the faces, one stage per plane, until
// each piece is on one face. 'vecs' must have room for one corner past 'nump': the cut writes a
// copy of the first there, so the edge walk can read i + 1 without a modulo.
void ClipSkyPolygon(const int nump, vec3_t * vecs, const int stage)
{
    if (nump > kMaxSkyClipVerts - 2)
    {
        Con_DPrintf("ClipSkyPolygon: %d corners, sky polygon dropped\n", nump);
        return;
    }
    if (stage == kSkyClipStages)
    {
        ProjectSkyPolygon(nump, vecs);
        return;
    }

    const float * const norm = s_skyClip[stage];

    enum Side : u8 { kFront, kBack, kOn };
    float dists[kMaxSkyClipVerts];
    Side  sides[kMaxSkyClipVerts];

    bool front = false;
    bool back  = false;
    for (int i = 0; i < nump; ++i)
    {
        const float d = DotProduct(vecs[i], norm);
        if (d > kOnPlaneEpsilon)
        {
            front    = true;
            sides[i] = kFront;
        }
        else if (d < -kOnPlaneEpsilon)
        {
            back     = true;
            sides[i] = kBack;
        }
        else
        {
            sides[i] = kOn;
        }
        dists[i] = d;
    }

    if (!front || !back)
    {
        ClipSkyPolygon(nump, vecs, stage + 1); // all on one side: nothing to cut
        return;
    }

    sides[nump] = sides[0];
    dists[nump] = dists[0];
    VectorCopy(vecs[0], vecs[nump]);

    vec3_t (&newv)[2][kMaxSkyClipVerts] = s_skyClipVerts[stage];
    int newc[2] = { 0, 0 };

    for (int i = 0; i < nump; ++i)
    {
        const float * const v = vecs[i];

        switch (sides[i])
        {
        case kFront:
            VectorCopy(v, newv[0][newc[0]]);
            ++newc[0];
            break;
        case kBack:
            VectorCopy(v, newv[1][newc[1]]);
            ++newc[1];
            break;
        case kOn:
            VectorCopy(v, newv[0][newc[0]]);
            ++newc[0];
            VectorCopy(v, newv[1][newc[1]]);
            ++newc[1];
            break;
        }

        if (sides[i] == kOn || sides[i + 1] == kOn || sides[i + 1] == sides[i])
        {
            continue;
        }

        const float frac = dists[i] / (dists[i] - dists[i + 1]);
        for (int j = 0; j < 3; ++j)
        {
            const float e = v[j] + (frac * (vecs[i + 1][j] - v[j]));
            newv[0][newc[0]][j] = e;
            newv[1][newc[1]][j] = e;
        }
        ++newc[0];
        ++newc[1];
    }

    ClipSkyPolygon(newc[0], newv[0], stage + 1);
    ClipSkyPolygon(newc[1], newv[1], stage + 1);
}

// The face the camera-relative direction 'v' points at: the dominant component's axis, its sign
// the side. -1 when two components tie for it, which leaves the face for the full cut to settle.
int FaceOf(const float * const v)
{
    const float ax = math::Fabsf(v[0]);
    const float ay = math::Fabsf(v[1]);
    const float az = math::Fabsf(v[2]);
    if (ax > ay && ax > az)
    {
        return (v[0] < 0.0f) ? 1 : 0;
    }
    if (ay > ax && ay > az)
    {
        return (v[1] < 0.0f) ? 3 : 2;
    }
    if (az > ax && az > ay)
    {
        return (v[2] < 0.0f) ? 5 : 4;
    }
    return -1;
}

// Grows the bounds by one sky surface, its corners in the model's space under 'toWorld', or in the
// world's when that is null: Sky_ProcessPoly's half that QuakeSpasm's bounds come from.
//
// Most sky polygons lie within one face's view: when every corner points at the same face, so
// does the whole polygon - a face's view is a convex cone, and so is the polygon - and its corners
// go straight to the projection, with none of the cut's six stages.
void AddSkySurface(const brush::SurfaceDraw & draw, const math::Mat4 * const toWorld)
{
    if (draw.geometry != brush::Geometry::Fan || draw.numVerts < 3 || draw.numVerts > kMaxSkyClipVerts - 2)
    {
        return;
    }

    vec3_t verts[kMaxSkyClipVerts];
    for (int i = 0; i < draw.numVerts; ++i)
    {
        const math::Vec3 & p = draw.verts[i].position;
        math::Vec3 world = p;
        if (toWorld != nullptr)
        {
            const math::Vec4 w = math::Transform(math::Vec4{ p.x, p.y, p.z, 1.0f }, *toWorld);
            world = { w.x, w.y, w.z };
        }
        verts[i][0] = world.x - r_origin[0];
        verts[i][1] = world.y - r_origin[1];
        verts[i][2] = world.z - r_origin[2];
    }

    const int face = FaceOf(verts[0]);
    bool oneFace = (face >= 0);
    for (int i = 1; i < draw.numVerts && oneFace; ++i)
    {
        oneFace = (FaceOf(verts[i]) == face);
    }

    if (oneFace)
    {
        ProjectSkyPolygon(draw.numVerts, verts);
    }
    else
    {
        ClipSkyPolygon(draw.numVerts, verts, 0);
    }
}

// ------------------------------------------------------------------------------------------------
// The frame's sky surfaces (Sky_ProcessTextureChains, Sky_ProcessEntities)
// ------------------------------------------------------------------------------------------------

// Whether a brush model has any sky surface at all, which next to none of a map's doors and lifts
// do: those are passed over before their transforms are worked out.
bool HasSkySurfaces(const qmodel_t & model)
{
    const msurface_t * surf = &model.surfaces[model.firstmodelsurface];
    for (int i = 0; i < model.nummodelsurfaces; ++i, ++surf)
    {
        if ((surf->flags & SURF_DRAWSKY) != 0)
        {
            return true;
        }
    }
    return false;
}

// A brush entity whose sky surfaces may show: one with any, in view and not invisible. Its model to
// world transform, as the view draws it (R_DrawBrushModel's flipped pitch included).
bool EntitySkyTransform(const entity_t & e, math::Mat4 * const outToWorld)
{
    if (e.model->type != mod_brush || !HasSkySurfaces(*e.model) || view::CullModelForEntity(e) ||
        e.alpha == ENTALPHA_ZERO)
    {
        return false;
    }
    const vec3_t angles = { -e.angles[PITCH], e.angles[YAW], e.angles[ROLL] };
    *outToWorld = view::EntityMatrix(e.origin, angles, e.scale);
    return true;
}

// Whether a brush entity's sky surface faces the camera, from 'modelorg', the camera in the
// model's space - R_DrawBrushModel's test.
bool FacesCamera(const msurface_t & surf, const vec3_t modelorg)
{
    constexpr float kBackfaceEpsilon = static_cast<float>(BACKFACE_EPSILON);

    const float dot = DotProduct(modelorg, surf.plane->normal) - surf.plane->dist;
    return ((surf.flags & SURF_PLANEBACK) != 0 && dot < -kBackfaceEpsilon) ||
           ((surf.flags & SURF_PLANEBACK) == 0 && dot >  kBackfaceEpsilon);
}

// Calls 'visit(draw, mvp, toWorld)' for every sky surface in view: the world's, off its texture
// chains (toWorld null), then those of the brush entities that face the camera - and 'endModel()'
// after each model's, before the transforms it was handed go: a stream holds its transform by
// pointer, so a pass gathering under them has to flush there.
template<typename Visit, typename EndModel>
void ForEachSkySurface(Visit & visit, EndModel & endModel)
{
    const math::Mat4 & viewProj = view::ViewProjection();

    if (r_drawworld.value != 0.0f)
    {
        const qmodel_t & world = *cl.worldmodel;
        for (int i = 0; i < world.numtextures; ++i)
        {
            const texture_t * const t = world.textures[i];
            if (t == nullptr || t->texturechains[chain_world] == nullptr ||
                (t->texturechains[chain_world]->flags & SURF_DRAWSKY) == 0)
            {
                continue;
            }
            for (const msurface_t * s = t->texturechains[chain_world]; s != nullptr; s = s->texturechain)
            {
                visit(brush::DrawFor(world, *s), viewProj, static_cast<const math::Mat4 *>(nullptr));
            }
        }
        endModel();
    }

    if (r_drawentities.value == 0.0f)
    {
        return;
    }

    for (int i = 0; i < cl_numvisedicts; ++i)
    {
        const entity_t & e = *cl_visedicts[i];

        math::Mat4 toWorld;
        if (!EntitySkyTransform(e, &toWorld))
        {
            continue;
        }

        // The camera in the model's space.
        vec3_t modelorg;
        VectorSubtract(r_refdef.vieworg, e.origin, modelorg);
        if (e.angles[0] != 0.0f || e.angles[1] != 0.0f || e.angles[2] != 0.0f)
        {
            vec3_t temp, forward, right, up, angles;
            VectorCopy(modelorg, temp);
            VectorCopy(e.angles, angles);
            AngleVectors(angles, forward, right, up);
            modelorg[0] =  DotProduct(temp, forward);
            modelorg[1] = -DotProduct(temp, right);
            modelorg[2] =  DotProduct(temp, up);
        }

        const qmodel_t & model = *e.model;
        const math::Mat4 mvp = toWorld * viewProj;
        const msurface_t * surf = &model.surfaces[model.firstmodelsurface];
        for (int j = 0; j < model.nummodelsurfaces; ++j, ++surf)
        {
            if ((surf->flags & SURF_DRAWSKY) != 0 && FacesCamera(*surf, modelorg))
            {
                visit(brush::DrawFor(model, *surf), mvp, &toWorld);
            }
        }
        endModel();
    }
}

// Appends a surface's fan, every corner in one colour, under 'mvp'.
void GatherFan(rs::TriangleStream & stream, const brush::SurfaceDraw & draw, const math::Mat4 & mvp,
               const u32 rgba)
{
    if (draw.geometry != brush::Geometry::Fan)
    {
        return;
    }

    stream.SetTransform(mvp);

    const vu1::DrawVertex * const src = draw.verts;
    vu1::DrawVertex * __restrict dst = stream.ReserveVerts((draw.numVerts - 2) * 3);
    for (int t = 1; t < draw.numVerts - 1; ++t)
    {
        vu1::CopyDrawVertex(dst[0], src[0]);
        vu1::CopyDrawVertex(dst[1], src[t]);
        vu1::CopyDrawVertex(dst[2], src[t + 1]);
        dst[0].rgba = rgba;
        dst[1].rgba = rgba;
        dst[2].rgba = rgba;
        dst += 3;
    }
    stream.CommitVerts(dst);
}

// ------------------------------------------------------------------------------------------------
// The layers (gl_sky.c's Sky_DrawSkyLayers, Sky_DrawFace, Sky_DrawFaceQuad, Sky_GetTexCoord)
// ------------------------------------------------------------------------------------------------

// How far a layer has scrolled, in texels: 'speed' a second, wrapped to the 128-texel layer while
// it is still a double, as QuakeSpasm wraps it.
float LayerScroll(const float speed)
{
    const double scroll = cl.time * static_cast<double>(speed);
    return static_cast<float>(scroll - (std::floor(scroll / 128.0) * 128.0));
}

// A grid vertex of face 'axis' at face-local (s, t) in [-1, 1], as the back layer draws it: its
// place on the box around the camera, and the layer's texture coordinates there - Sky_GetTexCoord's
// flattened sphere, the direction's height tripled and the result spread over the 128-texel layer.
void MakeGridVertex(vu1::DrawVertex & out, const float s, const float t, const int axis, const float scroll)
{
    const float b[3] = { s * kBoxSize, t * kBoxSize, kBoxSize };

    float dir[3];
    for (int j = 0; j < 3; ++j)
    {
        const int k = s_stToVec[axis][j];
        dir[j] = (k < 0) ? -b[-k - 1] : b[k - 1];
    }

    const float flatZ  = dir[2] * 3.0f;
    const float length = (6.0f * 63.0f) / math::Sqrtf((dir[0] * dir[0]) + (dir[1] * dir[1]) + (flatZ * flatZ));

    out.position   = { r_origin[0] + dir[0], r_origin[1] + dir[1], r_origin[2] + dir[2] };
    out.lightmap_s = 0.0f;
    out.rgba       = kModulateIdentity;
    out.s          = (scroll + (dir[0] * length)) * (1.0f / 128.0f);
    out.t          = (scroll + (dir[1] * length)) * (1.0f / 128.0f);
    out.lightmap_t = 0.0f;
}

// Appends the grid's cells, two triangles each: QuakeSpasm's quad (i, j), (i, j + 1),
// (i + 1, j + 1), (i + 1, j), fanned.
void EmitGridCells(rs::TriangleStream & stream, const int columns, const int rows)
{
    for (int j = 0; j < rows - 1; ++j)
    {
        const vu1::DrawVertex * const row0 = &s_grid[j * columns];
        const vu1::DrawVertex * const row1 = &s_grid[(j + 1) * columns];

        vu1::DrawVertex * __restrict dst = stream.ReserveVerts((columns - 1) * 6);
        for (int i = 0; i < columns - 1; ++i)
        {
            vu1::CopyDrawVertex(dst[0], row0[i]);
            vu1::CopyDrawVertex(dst[1], row1[i]);
            vu1::CopyDrawVertex(dst[2], row1[i + 1]);
            vu1::CopyDrawVertex(dst[3], row0[i]);
            vu1::CopyDrawVertex(dst[4], row1[i + 1]);
            vu1::CopyDrawVertex(dst[5], row0[i + 1]);
            dst += 6;
        }
        stream.CommitVerts(dst);
    }
}

// Draws one box face's cells that the sky surfaces' bounds touch, in both layers.
void DrawFace(rs::TriangleStream & stream, const int axis, const int quality, const float backScroll,
              const float frontScroll, const u32 frontColor)
{
    const int di = quality;
    const int dj = (axis < 4) ? quality * 2 : quality; // the sides are cut twice as finely up them
    const float qi = 1.0f / static_cast<float>(di);
    const float qj = 1.0f / static_cast<float>(dj);

    // The cells the bounds reach, as QuakeSpasm picks them.
    int iFirst = di, iLast = -1, jFirst = dj, jLast = -1;
    for (int i = 0; i < di; ++i)
    {
        const float at = static_cast<float>(i) * qi;
        if (at >= ((s_skyMins[0][axis] * 0.5f) + 0.5f) - qi && at <= (s_skyMaxs[0][axis] * 0.5f) + 0.5f)
        {
            iFirst = (i < iFirst) ? i : iFirst;
            iLast  = i;
        }
    }
    for (int j = 0; j < dj; ++j)
    {
        const float at = static_cast<float>(j) * qj;
        if (at >= ((s_skyMins[1][axis] * 0.5f) + 0.5f) - qj && at <= (s_skyMaxs[1][axis] * 0.5f) + 0.5f)
        {
            jFirst = (j < jFirst) ? j : jFirst;
            jLast  = j;
        }
    }
    if (iLast < iFirst || jLast < jFirst)
    {
        return;
    }

    // The back layer's grid over those cells.
    const int columns = iLast - iFirst + 2;
    const int rows    = jLast - jFirst + 2;
    for (int j = 0; j < rows; ++j)
    {
        for (int i = 0; i < columns; ++i)
        {
            MakeGridVertex(s_grid[(j * columns) + i],
                           -1.0f + (2.0f * static_cast<float>(i + iFirst) * qi),
                           -1.0f + (2.0f * static_cast<float>(j + jFirst) * qj), axis, backScroll);
        }
    }

    stream.SetTexture(*s_solidLayer);
    stream.SetDrawFlags(rs::DrawFlags::NoDepthWrite);
    EmitGridCells(stream, columns, rows);

    // The front layer over it: the same grid, scrolled further, in its own colour.
    const float offset = (frontScroll - backScroll) * (1.0f / 128.0f);
    for (int k = 0; k < rows * columns; ++k)
    {
        s_grid[k].s   += offset;
        s_grid[k].t   += offset;
        s_grid[k].rgba = frontColor;
    }

    stream.SetTexture(*s_alphaLayer);
    stream.SetDrawFlags(rs::DrawFlags::Blended);
    EmitGridCells(stream, columns, rows);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    Cvar_RegisterVariable(&s_fastSky);
    Cvar_RegisterVariable(&s_skyQuality);
    Cvar_RegisterVariable(&s_skyAlpha);

    // QuakeSpasm's Sky_SkyCommand_f, over skyboxes that never load here (see Sky_LoadSkyBox).
    Cmd_AddCommand("sky", []()
    {
        if (Cmd_Argc() == 1)
        {
            Con_Printf("\"sky\" is \"\"\n");
        }
        else if (Cmd_Argc() == 2)
        {
            Sky_LoadSkyBox(Cmd_Argv(1));
        }
        else
        {
            Con_Printf("usage: sky <skyname>\n");
        }
    });
}

// gl_sky.c's Sky_NewMap: worldspawn may name a skybox.
void NewMap()
{
    const char * data = COM_Parse(cl.worldmodel->entities);
    if (data == nullptr || com_token[0] != '{')
    {
        return;
    }

    for (;;)
    {
        data = COM_Parse(data);
        if (data == nullptr || com_token[0] == '}')
        {
            return;
        }

        char key[128];
        q_strlcpy(key, (com_token[0] == '_') ? (com_token + 1) : com_token, sizeof(key));
        for (size_t len = std::strlen(key); len > 0 && key[len - 1] == ' '; --len)
        {
            key[len - 1] = '\0';
        }

        data = COM_ParseEx(data, CPE_ALLOWTRUNC);
        if (data == nullptr)
        {
            return;
        }

        // QuakeSpasm's key, and the two other engines' it accepts too.
        if (std::strcmp(key, "sky") == 0 || std::strcmp(key, "skyname") == 0 || std::strcmp(key, "qlsky") == 0)
        {
            Sky_LoadSkyBox(com_token);
        }
    }
}

void Draw(rs::TriangleStream & stream)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Sky);

    // The flat sky: the surfaces themselves in the flat colour, writing their depth. Also what
    // r_lightmap draws, as QuakeSpasm leaves the sky to the world's passes there.
    const bool layers = s_fastSky.value == 0.0f && !view::LightmapMode() &&
                        s_solidLayer != nullptr && s_alphaLayer != nullptr;

    stream.SetTexture(tex::DebugTexture()); // Unsampled, but a batch binds one.

    auto flush = [&stream]() { rs::Submit(stream); };

    if (!layers)
    {
        stream.SetDrawFlags(rs::DrawFlags::Untextured);
        auto flat = [&stream](const brush::SurfaceDraw & draw, const math::Mat4 & mvp, const math::Mat4 *)
        {
            GatherFan(stream, draw, mvp, s_flatColor);
        };
        ForEachSkySurface(flat, flush);
        return;
    }

    // What of the box the sky surfaces in view cover.
    ClearBounds();
    auto bound = [](const brush::SurfaceDraw & draw, const math::Mat4 &, const math::Mat4 * toWorld)
    {
        AddSkySurface(draw, toWorld);
    };
    auto nothing = []() {};
    ForEachSkySurface(bound, nothing);
    if (!AnyBounds())
    {
        return; // indoors
    }

    // The layers over it, before anything else and with no depth: the front one at r_skyalpha,
    // which its RGBA texels' 0x80 multiplies straight into the GS's 1.0.
    const int quality = static_cast<int>(s_skyQuality.value);
    const int cells   = (quality < 1) ? 1 : ((quality > kMaxSkyQuality) ? kMaxSkyQuality : quality);

    const float alpha = (s_skyAlpha.value < 0.0f) ? 0.0f : ((s_skyAlpha.value > 1.0f) ? 1.0f : s_skyAlpha.value);
    const u32 frontColor = vu1::PackColorRGBA(128, 128, 128, static_cast<u32>(alpha * 128.0f));

    const float backScroll  = LayerScroll(8.0f);
    const float frontScroll = LayerScroll(16.0f);

    stream.SetTransform(view::ViewProjection());
    for (int axis = 0; axis < kNumFaces; ++axis)
    {
        if (s_skyMins[0][axis] < s_skyMaxs[0][axis] && s_skyMins[1][axis] < s_skyMaxs[1][axis])
        {
            DrawFace(stream, axis, cells, backScroll, frontScroll, frontColor);
        }
    }
    rs::Submit(stream);

    // Then the sky surfaces' depth, so what stands behind them stays hidden from the world drawn
    // next, and the layers show through them alone.
    stream.SetTexture(tex::DebugTexture());
    stream.SetDrawFlags(rs::DrawFlags::Untextured | rs::DrawFlags::DepthOnly);
    auto depth = [&stream](const brush::SurfaceDraw & draw, const math::Mat4 & mvp, const math::Mat4 *)
    {
        GatherFan(stream, draw, mvp, kModulateIdentity);
    };
    ForEachSkySurface(depth, flush);
}

} // namespace ps2::sky

extern "C" {

// ------------------------------------------------------------------------------------------------
// The engine's sky hooks
// ------------------------------------------------------------------------------------------------

// A sky texture is 256x128: on the right the solid back layer, on the left the front one, whose
// index 0 is a hole. Called as the map loads (Mod_LoadTextures).
void Sky_LoadTexture(qmodel_t * mod, texture_t * mt)
{
    using namespace ps2::sky;

    ClearLayers();

    if (mt->width != 256 || mt->height != 128)
    {
        Con_DPrintf("Sky texture %s is %d x %d, expected 256 x 128\n", mt->name, mt->width, mt->height);
    }

    const int width  = static_cast<int>(mt->width) / 2;
    const int height = static_cast<int>(mt->height);
    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height) || (width * height) < 16)
    {
        Con_DPrintf("Sky texture %s can't tile; the sky draws flat\n", mt->name);
        return;
    }

    const byte * const src = static_cast<const byte *>(static_cast<const void *>(mt + 1));
    const int stride = static_cast<int>(mt->width);

    CreateLayers(*mod, *mt, width, height,
        [src, stride, width](const int x, const int y) -> byte { return src[(y * stride) + width + x]; },
        [src, stride](const int x, const int y) -> u32
        {
            const byte p = src[(y * stride) + x];
            return (p == 0) ? 0u : ((d_8to24table[p] & 0x00FFFFFFu) | (0x80u << 24));
        });
}

// A Quake 64 sky is 32x64: the front layer on top, the back one below, and the front blended at
// half its opacity everywhere rather than cut out.
void Sky_LoadTextureQ64(qmodel_t * mod, texture_t * mt)
{
    using namespace ps2::sky;

    ClearLayers();

    const int width  = static_cast<int>(mt->width);
    const int height = static_cast<int>(mt->height) / 2;
    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height) || (width * height) < 16)
    {
        Con_DPrintf("Q64 sky texture %s can't tile; the sky draws flat\n", mt->name);
        return;
    }

    const byte * const front = static_cast<const byte *>(static_cast<const void *>(mt + 1));
    const byte * const back  = front + (width * height);

    CreateLayers(*mod, *mt, width, height,
        [back, width](const int x, const int y) -> byte { return back[(y * width) + x]; },
        [front, width](const int x, const int y) -> u32
        {
            return (d_8to24table[front[(y * width) + x]] & 0x00FFFFFFu) | (0x40u << 24);
        });
}

// A skybox is six images loaded from gfx/env/, which the PS2 doesn't load (see Image_LoadImage):
// the scrolling layers stay, as QuakeSpasm keeps them when a skybox is missing.
void Sky_LoadSkyBox(const char * name)
{
    if (name != nullptr && name[0] != '\0')
    {
        Con_Printf("Couldn't load skybox %s: no external images on the PS2\n", name);
    }
}

// The map is going: its layers with it.
void Sky_ClearAll()
{
    ps2::sky::ClearLayers();
}

} // extern "C"

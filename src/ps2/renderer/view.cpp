/* ================================================================================================
 * File: view.cpp
 * Brief: The 3D view: QuakeSpasm's R_RenderView on the VU1 path. See view.h.
 *
 *  Setup and visibility are QuakeSpasm's own, function for function: R_SetupView's dynamic lights,
 *  light styles, view leaf and frustum, and R_MarkSurfaces, which walks the leafs the PVS lets
 *  through, culls their surfaces against the frustum and their planes, and threads the survivors
 *  onto their textures' chains (texture_t::texturechains). Drawing walks those chains as
 *  QuakeSpasm's multipass path does, each pass one rs::TriangleStream:
 *
 *    - diffuse: the texture, at the modulate identity;
 *    - lightmap: the same triangles again through the atlas UVs, multiplying the framebuffer by
 *      the luxel (Modulate, Cd * As / 128, overbright up to nearly 2x) - QuakeSpasm's 2x
 *      modulate pass with gl_overbright;
 *    - fullbright: the texels 224-255 of the textures that have them, added over the lit result
 *      (Additive) - its gl_fullbrights glow pass.
 *
 *  Water and its kin draw after the opaque entities, as there, with their texture coordinates
 *  bent on VU1 (rs::DrawFlags::Warped). Brush entities - doors, lifts, the ammo boxes - run the
 *  same passes under their own transform. The sky draws flat for now, in the colour QuakeSpasm's
 *  r_fastsky uses.
 *
 *  Camera mapping: Quake is Z-up with AngleVectors giving forward/right/up; those feed
 *  math::LookAt directly, and the projection's Y flip puts +up up on the GS's screen.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/alias.h"
#include "ps2/renderer/brush.h"
#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/particles.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/sky.h"
#include "ps2/renderer/sprite.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/gs.h"
#include "ps2/math/vec_mat.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {

// sv_main.c's; QuakeSpasm's r_world.c declared it for itself too.
byte * SV_FatPVS(vec3_t org, qmodel_t * worldmodel);

// gl_rlight.c's: tints the view, for a dynamic light glow the camera is inside.
void AddLightBlend(float r, float g, float b, float a2);

// ------------------------------------------------------------------------------------------------
// Renderer state the engine headers declare (glquake.h) for gl_rmain.c to define
// ------------------------------------------------------------------------------------------------

mleaf_t * r_viewleaf      = nullptr;
mleaf_t * r_oldviewleaf   = nullptr;
int       r_visframecount = 0;   // Bumped by every R_MarkSurfaces; stamps the surfaces it reaches.
mplane_t  frustum[4];            // Left, right, bottom, top.

// The liquids' opacity: the cvars, or what the map's worldspawn says (R_ParseWorldspawn).
float map_wateralpha = 1.0f;
float map_lavaalpha  = 0.0f;
float map_telealpha  = 0.0f;
float map_slimealpha = 0.0f;

// ------------------------------------------------------------------------------------------------
// Cvars: QuakeSpasm's, at its defaults, for the passes there are so far
// ------------------------------------------------------------------------------------------------

cvar_t r_norefresh    = ps2::MakeCvar("r_norefresh",    "0", CVAR_NONE);
cvar_t r_drawentities = ps2::MakeCvar("r_drawentities", "1", CVAR_NONE);
cvar_t r_drawworld    = ps2::MakeCvar("r_drawworld",    "1", CVAR_NONE);
cvar_t r_fullbright   = ps2::MakeCvar("r_fullbright",   "0", CVAR_NONE);
cvar_t r_lightmap     = ps2::MakeCvar("r_lightmap",     "0", CVAR_NONE);
cvar_t r_wateralpha   = ps2::MakeCvar("r_wateralpha",   "1", CVAR_ARCHIVE);
cvar_t r_lavaalpha    = ps2::MakeCvar("r_lavaalpha",    "0", CVAR_NONE);
cvar_t r_telealpha    = ps2::MakeCvar("r_telealpha",    "0", CVAR_NONE);
cvar_t r_slimealpha   = ps2::MakeCvar("r_slimealpha",   "0", CVAR_NONE);
cvar_t r_dynamic      = ps2::MakeCvar("r_dynamic",      "1", CVAR_ARCHIVE);
cvar_t r_novis        = ps2::MakeCvar("r_novis",        "0", CVAR_ARCHIVE);
cvar_t r_oldskyleaf   = ps2::MakeCvar("r_oldskyleaf",   "0", CVAR_NONE);
cvar_t gl_fullbrights = ps2::MakeCvar("gl_fullbrights", "1", CVAR_ARCHIVE);
cvar_t gl_farclip     = ps2::MakeCvar("gl_farclip",     "65536", CVAR_ARCHIVE);
cvar_t r_waterwarp    = ps2::MakeCvar("r_waterwarp",    "1", CVAR_NONE);

} // extern "C"

namespace ps2::view {
namespace {

// ------------------------------------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------------------------------------

// QuakeSpasm's near plane (gl_rmain.c's NEARCLIP).
constexpr float kNearClip = 4.0f;

// How far in front of a brush entity's surface the camera has to be for it to face the camera
// (glquake.h's BACKFACE_EPSILON, a double there).
constexpr float kBackfaceEpsilon = static_cast<float>(BACKFACE_EPSILON);

// Vertices a pass gathers before it flushes a batch to VU1: 768 whole triangles.
constexpr int kBatchMaxVerts = 3 * 768;

// The GS modulate identity at full alpha: what a texel draws at unchanged.
constexpr u32 kModulateIdentity = vu1::PackColorRGBA(128, 128, 128, 0x80);

// What a vertex's alpha byte is for an opacity of 'alpha', 0 to 1, on the GS's 0x80 = 1.0 scale.
Q_ALWAYS_INLINE u32 AlphaByte(const float alpha)
{
    const float scaled = alpha * 128.0f;
    return (scaled >= 128.0f) ? 128u : ((scaled <= 0.0f) ? 0u : static_cast<u32>(scaled));
}

// ------------------------------------------------------------------------------------------------
// Frame state
// ------------------------------------------------------------------------------------------------

// World to clip: the frame's view-projection. World geometry draws under it as it is.
static math::Mat4 s_viewProj = {};

// QuakeSpasm's cheat-safe draw modes (R_SetupView): r_fullbright and r_lightmap only take in single
// player, and there a map without light data draws fullbright. The lightmap pass runs unless the
// mode is fullbright - or there are no lightmaps to draw.
static bool s_fullbrightMode = false;
static bool s_lightmapMode   = false;
static bool s_drawLightmaps  = true;

// ps2_mip_filter picks how the walls and model skins filter, by name, as QuakeSpasm's
// gl_texturemode does: nearest, bilinear (between texels, the nearest mip level) or trilinear
// (between levels too, at twice the texture reads). ps2_mip_bias shifts the mip levels the walls
// sample, in levels; positive is blurrier. See SetUpTextureSampling.
static cvar_t s_mipFilter = ps2::MakeCvar("ps2_mip_filter", "bilinear", CVAR_ARCHIVE);
static cvar_t s_mipBias   = ps2::MakeCvar("ps2_mip_bias",   "0",        CVAR_ARCHIVE);

static gs::MipFilter s_mipFilterMode = gs::MipFilter::Bilinear;

// ------------------------------------------------------------------------------------------------
// Culling (gl_rmain.c's R_CullBox, R_CullModelForEntity; r_world.c's R_BackFaceCull)
// ------------------------------------------------------------------------------------------------

int SignbitsForPlane(const mplane_t & plane)
{
    int bits = 0;
    for (int j = 0; j < 3; ++j)
    {
        if (plane.normal[j] < 0.0f)
        {
            bits |= (1 << j);
        }
    }
    return bits;
}

// Turns 'forward' towards 'side' by 'angle' degrees in the plane they span: gl_rmain.c's
// TurnVector. Both are unit length and perpendicular.
void TurnVector(vec3_t out, const vec3_t forward, const vec3_t side, const float angle)
{
    const float radians      = math::DegToRad(angle);
    const float scaleForward = math::Cosf(radians);
    const float scaleSide    = math::Sinf(radians);

    out[0] = (scaleForward * forward[0]) + (scaleSide * side[0]);
    out[1] = (scaleForward * forward[1]) + (scaleSide * side[1]);
    out[2] = (scaleForward * forward[2]) + (scaleSide * side[2]);
}

// The four side planes of the view frustum, for the bounding-box culls: gl_rmain.c's
// R_SetFrustum.
void SetFrustum(const float fovx, const float fovy)
{
    TurnVector(frustum[0].normal, vpn, vright, (fovx * 0.5f) - 90.0f); // left
    TurnVector(frustum[1].normal, vpn, vright, 90.0f - (fovx * 0.5f)); // right
    TurnVector(frustum[2].normal, vpn, vup,    90.0f - (fovy * 0.5f)); // bottom
    TurnVector(frustum[3].normal, vpn, vup,    (fovy * 0.5f) - 90.0f); // top

    for (mplane_t & plane : frustum)
    {
        plane.type     = PLANE_ANYZ;
        plane.dist     = DotProduct(r_origin, plane.normal);
        plane.signbits = static_cast<byte>(SignbitsForPlane(plane));
    }
}

// True when the box is entirely behind one of the frustum's side planes.
bool CullBox(const float * const mins, const float * const maxs)
{
    for (const mplane_t & plane : frustum)
    {
        const int signbits = plane.signbits;
        const float x = ((signbits & 1) ? mins : maxs)[0];
        const float y = ((signbits & 2) ? mins : maxs)[1];
        const float z = ((signbits & 4) ? mins : maxs)[2];
        if ((plane.normal[0] * x) + (plane.normal[1] * y) + (plane.normal[2] * z) < plane.dist)
        {
            return true;
        }
    }
    return false;
}

// The entity's model bounds placed where it stands, taking the rotated bounds gl_model.c
// precomputed when it yaws, pitches or rolls.
bool CullModelBounds(const entity_t & e)
{
    const float * minBounds;
    const float * maxBounds;
    if (e.angles[0] != 0.0f || e.angles[2] != 0.0f)
    {
        minBounds = e.model->rmins;
        maxBounds = e.model->rmaxs;
    }
    else if (e.angles[1] != 0.0f)
    {
        minBounds = e.model->ymins;
        maxBounds = e.model->ymaxs;
    }
    else
    {
        minBounds = e.model->mins;
        maxBounds = e.model->maxs;
    }

    const float scale = ENTSCALE_DECODE(e.scale);
    vec3_t mins, maxs;
    for (int i = 0; i < 3; ++i)
    {
        mins[i] = e.origin[i] + (minBounds[i] * scale);
        maxs[i] = e.origin[i] + (maxBounds[i] * scale);
    }
    return CullBox(mins, maxs);
}

// True when the camera is behind the surface's plane.
bool BackFaceCull(const msurface_t & surf)
{
    const mplane_t & plane = *surf.plane;
    const float dot = (plane.type < 3) ? (r_refdef.vieworg[plane.type] - plane.dist)
                                       : (DotProduct(r_refdef.vieworg, plane.normal) - plane.dist);
    return (dot < 0.0f) != ((surf.flags & SURF_PLANEBACK) != 0);
}

// ------------------------------------------------------------------------------------------------
// Transforms
// ------------------------------------------------------------------------------------------------

// The view-projection for the frame: QuakeSpasm's R_SetupGL - glFrustum over the field of view,
// glViewport over r_refdef.vrect - for the GS.
//
// The projection maps the view rectangle's extent onto NDC at the GS's scale (the 4096-unit
// drawing window doubling as the clip guard band, see math::PerspectiveProjection), and moves
// the projection's centre onto the rectangle's when it is off the screen's: NDC 1.0 is 2048
// pixels from the screen's centre.
void SetupTransforms(const float fovx, const float fovy)
{
    const math::Vec3 eye    = { r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2] };
    const math::Vec3 target = { eye.x + vpn[0], eye.y + vpn[1], eye.z + vpn[2] };
    const math::Vec3 up     = { vup[0], vup[1], vup[2] };
    const math::Mat4 view   = math::LookAt(eye, target, up);

    const vrect_t & rect = r_refdef.vrect;
    const float cotX = 1.0f / std::tan(math::DegToRad(fovx) * 0.5f);
    const float cotY = 1.0f / std::tan(math::DegToRad(fovy) * 0.5f);
    const float w    = cotX * (static_cast<float>(rect.width)  / 4096.0f);
    const float h    = cotY * (static_cast<float>(rect.height) / 4096.0f);

    const float centreX = static_cast<float>(rect.x) + (static_cast<float>(rect.width)  * 0.5f);
    const float centreY = static_cast<float>(rect.y) + (static_cast<float>(rect.height) * 0.5f);
    const float offsetX = (centreX - (static_cast<float>(gs::Width())  * 0.5f)) / 2048.0f;
    const float offsetY = (centreY - (static_cast<float>(gs::Height()) * 0.5f)) / 2048.0f;

    const float zNear = kNearClip;
    const float zFar  = (gl_farclip.value > (zNear * 2.0f)) ? gl_farclip.value : 65536.0f;

    // Row vectors (clip = eye * proj), looking down -Z: clip.w is -z, so the third row's x and y
    // add the centre's offset times w, which the divide leaves as a plain NDC offset.
    const math::Mat4 proj = {{
        { w,        0.0f,     0.0f,                                0.0f },
        { 0.0f,     -h,       0.0f,                                0.0f }, // Y flipped: GS screen space grows downwards
        { -offsetX, -offsetY, (zFar + zNear) / (zFar - zNear),     -1.0f },
        { 0.0f,     0.0f,     (2.0f * zFar * zNear) / (zFar - zNear), 0.0f }
    }};

    s_viewProj = view * proj;
}

// The frame's texture filtering, and the constant that picks the walls' mip levels.
//
// A wall texel spans one world unit, and at view depth w a world unit covers f / w pixels, where
// f = (view height / 2) * cot(fovY / 2) is the projection's focal length in pixels (320 at 640x448
// and a 70-degree vertical field of view). So texels shrink to a pixel at w = f, and the level that
// keeps them about a pixel wide is log2(w / f): with the GS measuring log2(1/Q) and Q = 1/w, that
// makes K = -log2(f). The GS goes by depth alone, not by how slanted the surface is, so a floor
// seen at a grazing angle gets a sharper level than its texel density on screen calls for;
// ps2_mip_bias is the knob for that.
void SetUpTextureSampling(const float fovy)
{
    const float halfFovY    = math::DegToRad(fovy) * 0.5f;
    const float focalPixels = 0.5f * static_cast<float>(r_refdef.vrect.height) * math::Cosf(halfFovY) / math::Sinf(halfFovY);
    const float lodK        = s_mipBias.value - std::log2(focalPixels);

    // TEX1.K is signed fixed point with four fraction bits, in 12 bits.
    int lodK16 = static_cast<int>((lodK * 16.0f) + ((lodK < 0.0f) ? -0.5f : 0.5f));
    lodK16 = (lodK16 < -2048) ? -2048 : ((lodK16 > 2047) ? 2047 : lodK16);

    rs::SetTextureSampling({ s_mipFilterMode, lodK16 });
}

// ps2_mip_filter's callback: takes the new name, or puts the old one back.
void MipFilterChanged(cvar_t * var)
{
    static const char * const kFilterNames[] = { "nearest", "bilinear", "trilinear" };
    for (int i = 0; i < ps2::ArrayLength(kFilterNames); ++i)
    {
        if (q_strcasecmp(var->string, kFilterNames[i]) == 0)
        {
            s_mipFilterMode = static_cast<gs::MipFilter>(i);
            return;
        }
    }

    const char * const current = kFilterNames[static_cast<int>(s_mipFilterMode)];
    Con_Printf("ps2_mip_filter: '%s' is not nearest, bilinear or trilinear; keeping %s.\n", var->string, current);
    Cvar_SetQuick(var, current);
}

// ------------------------------------------------------------------------------------------------
// Visibility (r_world.c's R_MarkSurfaces)
// ------------------------------------------------------------------------------------------------

// Threads a visible surface onto its texture's chain: r_world.c's R_ChainSurface.
Q_ALWAYS_INLINE void ChainSurface(msurface_t & surf, const texchain_t chain)
{
    surf.texturechain = surf.texinfo->texture->texturechains[chain];
    surf.texinfo->texture->texturechains[chain] = &surf;
}

void ClearTextureChains(qmodel_t & model, const texchain_t chain)
{
    for (int i = 0; i < model.numtextures; ++i)
    {
        if (model.textures[i] != nullptr)
        {
            model.textures[i]->texturechains[chain] = nullptr;
        }
    }
}

// Marks the surfaces of every leaf the PVS lets through and the frustum doesn't cull, chains the
// ones facing the camera, rebuilds their lightmaps if their lighting moved, and adds the static
// entities standing in those leafs to the frame's entity list.
void MarkSurfaces()
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Vis);

    qmodel_t & world = *cl.worldmodel;

    // Seen through a water surface the camera is next to, the other side is in a leaf the
    // camera's own PVS may not reach: take the PVS of everything around the eye then.
    bool nearWaterPortal = false;
    msurface_t ** mark = r_viewleaf->firstmarksurface;
    for (int i = 0; i < r_viewleaf->nummarksurfaces; ++i, ++mark)
    {
        if (((*mark)->flags & SURF_DRAWTURB) != 0)
        {
            nearWaterPortal = true;
        }
    }

    const byte * vis;
    if (r_novis.value != 0.0f || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
    {
        vis = Mod_NoVisPVS(&world);
    }
    else if (nearWaterPortal)
    {
        vis = SV_FatPVS(r_origin, &world);
    }
    else
    {
        vis = Mod_LeafPVS(r_viewleaf, &world);
    }

    ++r_visframecount;
    ClearTextureChains(world, chain_world);

    mleaf_t * leaf = &world.leafs[1];
    for (int i = 0; i < world.numleafs; ++i, ++leaf)
    {
        if ((vis[i >> 3] & (1 << (i & 7))) == 0)
        {
            continue;
        }
        if (CullBox(leaf->minmaxs, leaf->minmaxs + 3))
        {
            continue;
        }

        if (r_oldskyleaf.value != 0.0f || leaf->contents != CONTENTS_SKY)
        {
            mark = leaf->firstmarksurface;
            for (int j = 0; j < leaf->nummarksurfaces; ++j, ++mark)
            {
                msurface_t & surf = **mark;
                if (surf.visframe == r_visframecount)
                {
                    continue; // Already reached through another leaf.
                }
                surf.visframe = r_visframecount;

                if (!CullBox(surf.mins, surf.maxs) && !BackFaceCull(surf))
                {
                    ChainSurface(surf, chain_world);
                    if ((surf.flags & SURF_DRAWTILED) == 0)
                    {
                        lm::UpdateSurface(surf);
                    }
                }
            }
        }

        // The static entities in the leaf, onto cl_visedicts.
        if (leaf->efrags != nullptr)
        {
            R_StoreEfrags(&leaf->efrags);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Gathering
// ------------------------------------------------------------------------------------------------

// Appends a surface's triangles to the stream as they were baked: a fan from its first corner,
// or a turbulent surface's triangle list.
//
// The whole surface's room claimed once and written through a cursor of our own (see
// rs::TriangleStream::ReserveVerts), each vertex copied in two quadword moves.
void GatherSurface(rs::TriangleStream & stream, const brush::SurfaceDraw & draw)
{
    const vu1::DrawVertex * const src = draw.verts;

    if (draw.geometry == brush::Geometry::Triangles)
    {
        vu1::DrawVertex * __restrict dst = stream.ReserveVerts(draw.numVerts);
        for (int i = 0; i < draw.numVerts; ++i)
        {
            vu1::CopyDrawVertex(dst[i], src[i]);
        }
        stream.CommitVerts(dst + draw.numVerts);
        return;
    }

    if (draw.geometry != brush::Geometry::Fan)
    {
        return;
    }

    const int numVerts = draw.numVerts;
    vu1::DrawVertex * __restrict dst = stream.ReserveVerts((numVerts - 2) * 3);
    for (int t = 1; t < numVerts - 1; ++t)
    {
        vu1::CopyDrawVertex(dst[0], src[0]);
        vu1::CopyDrawVertex(dst[1], src[t]);
        vu1::CopyDrawVertex(dst[2], src[t + 1]);
        dst += 3;
    }
    stream.CommitVerts(dst);
}

// As GatherSurface, but every vertex takes 'rgba' instead of the baked colour: a translucent
// entity's or liquid's alpha, the flat sky.
void GatherSurfaceColored(rs::TriangleStream & stream, const brush::SurfaceDraw & draw, const u32 rgba)
{
    const vu1::DrawVertex * const src = draw.verts;

    if (draw.geometry == brush::Geometry::Triangles)
    {
        vu1::DrawVertex * __restrict dst = stream.ReserveVerts(draw.numVerts);
        for (int i = 0; i < draw.numVerts; ++i)
        {
            vu1::CopyDrawVertex(dst[i], src[i]);
            dst[i].rgba = rgba;
        }
        stream.CommitVerts(dst + draw.numVerts);
        return;
    }

    if (draw.geometry != brush::Geometry::Fan)
    {
        return;
    }

    const int numVerts = draw.numVerts;
    vu1::DrawVertex * __restrict dst = stream.ReserveVerts((numVerts - 2) * 3);
    for (int t = 1; t < numVerts - 1; ++t)
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

// A lit surface's triangles through its lightmap UVs instead of its diffuse ones, for the pass
// that multiplies the framebuffer by the luxels.
void GatherSurfaceLightmap(rs::TriangleStream & stream, const brush::SurfaceDraw & draw)
{
    PS2_Assert(draw.geometry == brush::Geometry::Fan);

    const vu1::DrawVertex * const src = draw.verts;
    const int numVerts = draw.numVerts;

    vu1::DrawVertex * __restrict dst = stream.ReserveVerts((numVerts - 2) * 3);
    for (int t = 1; t < numVerts - 1; ++t)
    {
        const int corners[3] = { 0, t, t + 1 };
        for (int i = 0; i < 3; ++i)
        {
            const vu1::DrawVertex & v = src[corners[i]];
            vu1::CopyDrawVertex(dst[i], v);
            dst[i].s = v.lightmap_s;
            dst[i].t = v.lightmap_t;
        }
        dst += 3;
    }
    stream.CommitVerts(dst);
}

// ------------------------------------------------------------------------------------------------
// Texture chains (r_world.c's R_DrawTextureChains, R_DrawTextureChains_Water)
// ------------------------------------------------------------------------------------------------

// The texture a surface of base texture 't' draws with this frame: r_brush.c's
// R_TextureAnimation. Animated walls ('+0' to '+9') cycle on the client's clock, ten frames a
// second; an entity in its alternate frame ('+a' to '+j') takes the alternate cycle.
const texture_t * TextureAnimation(const texture_t * base, const int frame)
{
    if (frame != 0 && base->alternate_anims != nullptr)
    {
        base = base->alternate_anims;
    }
    if (base->anim_total == 0)
    {
        return base;
    }

    const int relative = static_cast<int>(cl.time * 10.0) % base->anim_total;

    int count = 0;
    while (base->anim_min > relative || base->anim_max <= relative)
    {
        base = base->anim_next;
        if (base == nullptr)
        {
            Sys_Error("R_TextureAnimation: broken cycle");
        }
        if (++count > 100)
        {
            Sys_Error("R_TextureAnimation: infinite cycle");
        }
    }
    return base;
}

// The PS2 texture to bind for a gltexture, or the stand-in checkerboard when there is none (a
// texture missing from the BSP).
Q_ALWAYS_INLINE const tex::Texture & BindableTexture(const gltexture_t * gl)
{
    const tex::Texture * const texture = tex::TextureFor(gl);
    return (texture != nullptr) ? *texture : tex::DebugTexture();
}

// The surfaces at the head of a texture's chain decide which pass the whole chain belongs to: a
// texture is only ever sky, liquid or wall.
Q_ALWAYS_INLINE int ChainFlags(const texture_t * t, const texchain_t chain)
{
    return (t != nullptr && t->texturechains[chain] != nullptr) ? t->texturechains[chain]->flags : -1;
}

// The model's wall chains, in up to three passes: the textures, the lightmaps over them, and the
// fullbright texels over that. 'ent' is null for the world. QuakeSpasm's R_DrawTextureChains on
// its multipass path ("case 3"), with gl_overbright on.
void DrawTextureChains(rs::TriangleStream & stream, qmodel_t & model, const entity_t * ent,
                       const texchain_t chain, const math::Mat4 & mvp)
{
    const float entAlpha   = (ent != nullptr) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;
    const bool  translucent = (entAlpha < 1.0f);
    const int   frame       = (ent != nullptr) ? ent->frame : 0;

    // A translucent entity draws its textures alone, as QuakeSpasm's can't multipass one either.
    const bool lightmaps = s_drawLightmaps && !translucent;

    // Each atlas's visible surfaces, for the lightmap pass, gathered as the diffuse one goes.
    msurface_t * atlasChains[lm::kMaxAtlases] = {};

    // The diffuse pass.
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::TexChains);

        const u32 rgba = translucent ? vu1::PackColorRGBA(128, 128, 128, AlphaByte(entAlpha))
                                     : kModulateIdentity;

        stream.SetTransform(mvp);
        if (s_lightmapMode)
        {
            // r_lightmap: the walls flat white, so the lightmap pass over them shows the light alone.
            stream.SetDrawFlags(rs::DrawFlags::Untextured);
        }
        else
        {
            stream.SetDrawFlags(translucent ? rs::DrawFlags::Blended : rs::DrawFlags::None);
        }

        for (int i = 0; i < model.numtextures; ++i)
        {
            texture_t * const t = model.textures[i];
            const int flags = ChainFlags(t, chain);
            if (flags < 0 || (flags & (SURF_DRAWSKY | SURF_DRAWTURB)) != 0)
            {
                continue;
            }

            const texture_t * const anim = TextureAnimation(t, frame);
            stream.SetTexture(BindableTexture(anim->gltexture));

            for (msurface_t * s = t->texturechains[chain]; s != nullptr; s = s->texturechain)
            {
                brush::SurfaceDraw & draw = brush::DrawFor(model, *s);

                if (s_lightmapMode)
                {
                    GatherSurfaceColored(stream, draw, vu1::PackColorRGBA(255, 255, 255, 0x80));
                }
                else if (translucent)
                {
                    GatherSurfaceColored(stream, draw, rgba);
                }
                else
                {
                    GatherSurface(stream, draw);
                }

                if (lightmaps && (s->flags & SURF_DRAWTILED) == 0 && draw.geometry == brush::Geometry::Fan)
                {
                    draw.lightmapChain = atlasChains[s->lightmaptexturenum];
                    atlasChains[s->lightmaptexturenum] = s;
                }
            }
        }
        rs::Submit(stream);
    }

    // The lightmap pass: the same triangles over what the diffuse pass laid down, multiplying it by
    // the luxels. Depth writes are masked and the z-test is GREATER_EQUAL, so it covers exactly what
    // the first pass wrote.
    if (lightmaps)
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::LmChains);

        stream.SetTransform(mvp);
        stream.SetDrawFlags(rs::DrawFlags::Modulate);

        const int numAtlases = lm::NumAtlases();
        for (int a = 0; a < numAtlases; ++a)
        {
            if (atlasChains[a] == nullptr)
            {
                continue;
            }

            stream.SetTexture(lm::AtlasTexture(a));
            for (const msurface_t * s = atlasChains[a]; s != nullptr; )
            {
                const brush::SurfaceDraw & draw = brush::DrawFor(model, *s);
                GatherSurfaceLightmap(stream, draw);
                s = draw.lightmapChain;
            }
        }
        rs::Submit(stream);
    }

    // The fullbright pass: the texels in the fullbright range, added over the lit walls at their
    // own colour, which QuakeSpasm's no-bright palette left black in the first pass.
    if (gl_fullbrights.value != 0.0f && !s_lightmapMode)
    {
        stream.SetTransform(mvp);
        stream.SetDrawFlags(rs::DrawFlags::Additive);

        const u32 rgba = vu1::PackColorRGBA(128, 128, 128, AlphaByte(entAlpha));

        for (int i = 0; i < model.numtextures; ++i)
        {
            texture_t * const t = model.textures[i];
            const int flags = ChainFlags(t, chain);
            if (flags < 0 || (flags & (SURF_DRAWSKY | SURF_DRAWTURB)) != 0)
            {
                continue;
            }

            const texture_t * const anim = TextureAnimation(t, frame);
            const tex::Texture * const glow = tex::TextureFor(anim->fullbright);
            if (glow == nullptr)
            {
                continue;
            }

            stream.SetTexture(*glow);
            for (const msurface_t * s = t->texturechains[chain]; s != nullptr; s = s->texturechain)
            {
                GatherSurfaceColored(stream, brush::DrawFor(model, *s), rgba);
            }
        }
        rs::Submit(stream);
    }
}

// The opacity a liquid surface draws at: the entity's own alpha if it has one, else the map's or
// the cvar's for its kind (gl_rmisc.c's GL_WaterAlphaForSurface).
float WaterAlpha(const entity_t * ent, const msurface_t & surf)
{
    if (ent != nullptr && ent->alpha != ENTALPHA_DEFAULT)
    {
        return ENTALPHA_DECODE(ent->alpha);
    }
    if ((surf.flags & SURF_DRAWLAVA) != 0)
    {
        return (map_lavaalpha > 0.0f) ? map_lavaalpha : map_wateralpha;
    }
    if ((surf.flags & SURF_DRAWTELE) != 0)
    {
        return (map_telealpha > 0.0f) ? map_telealpha : map_wateralpha;
    }
    if ((surf.flags & SURF_DRAWSLIME) != 0)
    {
        return (map_slimealpha > 0.0f) ? map_slimealpha : map_wateralpha;
    }
    return map_wateralpha;
}

// The model's liquid surfaces, their texture coordinates bent on VU1, blended when they are
// translucent: R_DrawTextureChains_Water with r_oldwater's subdivided polygons.
void DrawWaterChains(rs::TriangleStream & stream, qmodel_t & model, const entity_t * ent,
                     const texchain_t chain, const math::Mat4 & mvp)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::TurbSurfs);

    stream.SetTransform(mvp);

    for (int i = 0; i < model.numtextures; ++i)
    {
        texture_t * const t = model.textures[i];
        const int flags = ChainFlags(t, chain);
        if (flags < 0 || (flags & SURF_DRAWTURB) == 0)
        {
            continue;
        }

        // The chain's first surface decides the opacity for all of it, as in QuakeSpasm.
        const float alpha = WaterAlpha(ent, *t->texturechains[chain]);
        const bool  blended = (alpha < 1.0f);

        stream.SetDrawFlags(blended ? (rs::DrawFlags::Warped | rs::DrawFlags::Blended) : rs::DrawFlags::Warped);
        stream.SetTexture(BindableTexture(t->gltexture));

        const u32 rgba = vu1::PackColorRGBA(128, 128, 128, AlphaByte(alpha));
        for (const msurface_t * s = t->texturechains[chain]; s != nullptr; s = s->texturechain)
        {
            const brush::SurfaceDraw & draw = brush::DrawFor(model, *s);
            if (blended)
            {
                GatherSurfaceColored(stream, draw, rgba);
            }
            else
            {
                GatherSurface(stream, draw);
            }
        }
    }
    rs::Submit(stream);
}

// ------------------------------------------------------------------------------------------------
// Entities
// ------------------------------------------------------------------------------------------------

// A brush model entity - a door, a lift, an ammo box: r_brush.c's R_DrawBrushModel. Its surfaces
// facing the camera chain onto the model's own texture chains and draw through the same passes
// as the world, under the entity's transform.
void DrawBrushModel(rs::TriangleStream & stream, entity_t & e)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::EntBrush);

    if (CullModelBounds(e))
    {
        return;
    }

    qmodel_t & model = *e.model;

    // The camera in the model's space, for the per-surface side test.
    vec3_t modelorg;
    VectorSubtract(r_refdef.vieworg, e.origin, modelorg);
    if (e.angles[0] != 0.0f || e.angles[1] != 0.0f || e.angles[2] != 0.0f)
    {
        vec3_t temp, forward, right, up;
        VectorCopy(modelorg, temp);
        AngleVectors(e.angles, forward, right, up);
        modelorg[0] =  DotProduct(temp, forward);
        modelorg[1] = -DotProduct(temp, right);
        modelorg[2] =  DotProduct(temp, up);
    }

    // Dynamic lights on the model's own surfaces, unless it is an instanced model (the ammo boxes,
    // whose surfaces every copy shares).
    if (model.firstmodelsurface != 0 && gl_flashblend.value == 0.0f)
    {
        // The lights' die times are float, so they're tested against the clock as a float: against
        // the double cl.time, every test was a soft-float call, 64 per brush model drawn.
        const float now = static_cast<float>(cl.time);
        for (int k = 0; k < MAX_DLIGHTS; ++k)
        {
            if (cl_dlights[k].die < now || cl_dlights[k].radius == 0.0f)
            {
                continue;
            }
            R_MarkLights(&cl_dlights[k], k, model.nodes + model.hulls[0].firstclipnode);
        }
    }

    // gl_rmain.c's R_RotateForEntity with the pitch flipped going in: R_DrawBrushModel's "stupid
    // quake bug", which brush models carry and alias models do not.
    const vec3_t angles = { -e.angles[PITCH], e.angles[YAW], e.angles[ROLL] };
    const math::Mat4 mvp = EntityMatrix(e.origin, angles, e.scale) * s_viewProj;

    ClearTextureChains(model, chain_model);

    msurface_t * surf = &model.surfaces[model.firstmodelsurface];
    for (int i = 0; i < model.nummodelsurfaces; ++i, ++surf)
    {
        const mplane_t & plane = *surf->plane;
        const float dot = DotProduct(modelorg, plane.normal) - plane.dist;
        const bool  back = (surf->flags & SURF_PLANEBACK) != 0;
        if ((back && dot < -kBackfaceEpsilon) || (!back && dot > kBackfaceEpsilon))
        {
            ChainSurface(*surf, chain_model);
            if ((surf->flags & SURF_DRAWTILED) == 0)
            {
                lm::UpdateSurface(*surf);
            }
        }
    }

    // Its sky surfaces went with the world's, ahead of everything (sky::Draw).
    DrawTextureChains(stream, model, &e, chain_model, mvp);
    DrawWaterChains(stream, model, &e, chain_model, mvp);
}

// The frame's entities: gl_rmain.c's R_DrawEntitiesOnList, one pass for the opaque ones and one,
// after the water, for the translucent ones.
//
// Sprites gather into the view's stream; an alias model draws through a stream of its own, which
// can only claim the command buffer once the view's has let go of it, hence the submit ahead of one.
void DrawEntitiesOnList(rs::TriangleStream & stream, const bool alphaPass)
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Entities);

    if (r_drawentities.value == 0.0f)
    {
        return;
    }

    for (int i = 0; i < cl_numvisedicts; ++i)
    {
        entity_t & e = *cl_visedicts[i];

        const bool translucent = ENTALPHA_DECODE(e.alpha) < 1.0f;
        if (translucent != alphaPass)
        {
            continue;
        }

        // The chase camera's view of the player leans back less (QuakeSpasm's chasecam).
        if (&e == &cl_entities[cl.viewentity])
        {
            e.angles[PITCH] *= 0.3f;
        }

        switch (e.model->type)
        {
        case mod_alias:
            rs::Submit(stream);
            alias::DrawAliasModel(e, /*viewModel=*/false);
            break;
        case mod_brush:
            DrawBrushModel(stream, e);
            break;
        case mod_sprite:
            sprite::DrawSpriteModel(stream, e);
            break;
        }
    }
    rs::Submit(stream);
}

// The view weapon: gl_rmain.c's R_DrawViewModel. Not while the chase camera is on, nor while the
// player is invisible or dead.
void DrawViewModel()
{
    if (r_drawviewmodel.value == 0.0f || r_drawentities.value == 0.0f || chase_active.value != 0.0f)
    {
        return;
    }
    if ((cl.items & IT_INVISIBILITY) != 0 || cl.stats[STAT_HEALTH] <= 0)
    {
        return;
    }

    entity_t & e = cl.viewent;
    if (e.model == nullptr || e.model->type != mod_alias)
    {
        return;
    }
    alias::DrawAliasModel(e, /*viewModel=*/true);
}

// ------------------------------------------------------------------------------------------------
// Dynamic light glows (gl_rlight.c's R_RenderDlights)
// ------------------------------------------------------------------------------------------------

// The points round a glow's rim, as gl_rlight.c's R_RenderDlight steps them: 16 segments, the last
// point the first again.
constexpr int kGlowSegments = 16;

struct GlowRim
{
    float cosines[kGlowSegments + 1];
    float sines[kGlowSegments + 1];
};

const GlowRim & GlowRimPoints()
{
    static GlowRim s_rim;
    static bool s_built = false;
    if (!s_built)
    {
        for (int i = 0; i <= kGlowSegments; ++i)
        {
            const float a = (static_cast<float>(i) / static_cast<float>(kGlowSegments)) * 2.0f * math::kPI;
            s_rim.cosines[i] = math::Cosf(a);
            s_rim.sines[i]   = math::Sinf(a);
        }
        s_built = true;
    }
    return s_rim;
}

// gl_flashblend's glows, in place of lighting the walls with the dynamic lights: each a fan a third
// of the light's radius across, orange at its centre - pulled towards the camera - and black at its
// rim, added over the scene. With the camera inside one, the screen takes an orange tint instead.
void RenderDlights(rs::TriangleStream & stream)
{
    if (gl_flashblend.value == 0.0f)
    {
        return;
    }

    constexpr u32 kCentreColor = vu1::PackColorRGBA(51, 26, 0, 0x80); // QuakeSpasm's (0.2, 0.1, 0)
    constexpr u32 kRimColor    = vu1::PackColorRGBA(0, 0, 0, 0x80);

    const GlowRim & rim = GlowRimPoints();
    const float now = static_cast<float>(cl.time);

    stream.SetTransform(s_viewProj);
    stream.SetDrawFlags(rs::DrawFlags::Untextured | rs::DrawFlags::Additive);
    stream.SetTexture(tex::DebugTexture()); // Unsampled, but a batch binds one.

    for (int i = 0; i < MAX_DLIGHTS; ++i)
    {
        const dlight_t & light = cl_dlights[i];
        if (light.die < now || light.radius == 0.0f)
        {
            continue;
        }

        const float rad = light.radius * 0.35f;

        vec3_t toLight;
        VectorSubtract(light.origin, r_origin, toLight);
        if (VectorLength(toLight) < rad)
        {
            AddLightBlend(1.0f, 0.5f, 0.0f, light.radius * 0.0003f); // the view is inside it
            continue;
        }

        vu1::DrawVertex centre = {};
        centre.position = { light.origin[0] - (vpn[0] * rad), light.origin[1] - (vpn[1] * rad),
                            light.origin[2] - (vpn[2] * rad) };
        centre.rgba = kCentreColor;

        vu1::DrawVertex points[kGlowSegments + 1] = {};
        for (int k = 0; k <= kGlowSegments; ++k)
        {
            const float c = rim.cosines[kGlowSegments - k] * rad;
            const float sn = rim.sines[kGlowSegments - k] * rad;
            points[k].position = { light.origin[0] + (vright[0] * c) + (vup[0] * sn),
                                   light.origin[1] + (vright[1] * c) + (vup[1] * sn),
                                   light.origin[2] + (vright[2] * c) + (vup[2] * sn) };
            points[k].rgba = kRimColor;
        }

        vu1::DrawVertex * dst = stream.ReserveVerts(kGlowSegments * 3);
        for (int k = 0; k < kGlowSegments; ++k)
        {
            vu1::CopyDrawVertex(dst[0], centre);
            vu1::CopyDrawVertex(dst[1], points[k]);
            vu1::CopyDrawVertex(dst[2], points[k + 1]);
            dst += 3;
        }
        stream.CommitVerts(dst);
    }
    rs::Submit(stream);
}

// ------------------------------------------------------------------------------------------------
// Frame setup (gl_rmain.c's R_SetupView)
// ------------------------------------------------------------------------------------------------

void SetupView()
{
    // Before the surfaces are marked, which rebuilds the lightmaps they touch.
    R_PushDlights();
    R_AnimateLight();
    ++r_framecount;

    VectorCopy(r_refdef.vieworg, r_origin);
    AngleVectors(r_refdef.viewangles, vpn, vright, vup);

    r_oldviewleaf = r_viewleaf;
    r_viewleaf    = Mod_PointInLeaf(r_origin, cl.worldmodel);

    V_SetContentsColor(r_viewleaf->contents);
    V_CalcBlend();

    // Under water, slime or lava, r_waterwarp sways the field of view.
    float fovx = r_refdef.fov_x;
    float fovy = r_refdef.fov_y;
    if (r_waterwarp.value != 0.0f)
    {
        const int contents = r_viewleaf->contents;
        if (contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA)
        {
            const float sway = math::Sinf(static_cast<float>(cl.time) * 1.5f) * 0.03f;
            fovx = 2.0f * math::RadToDeg(std::atan(std::tan(math::DegToRad(r_refdef.fov_x) * 0.5f) * (0.97f + sway)));
            fovy = 2.0f * math::RadToDeg(std::atan(std::tan(math::DegToRad(r_refdef.fov_y) * 0.5f) * (1.03f - sway)));
        }
    }

    SetFrustum(fovx, fovy);
    SetupTransforms(fovx, fovy);
    SetUpTextureSampling(fovy);

    // The water's ripple moves at a radian a second (see vu1::kTurbSinAmplitude). The frame's phase
    // goes to VU1 in turns, wrapped while it is still a double, so it keeps its precision however
    // long the map has been running.
    const double warpTurns = cl.time * (1.0 / (2.0 * M_PI));
    rs::SetWarpAnimation(static_cast<float>(warpTurns - std::floor(warpTurns)), 0.0f);

    MarkSurfaces();
    alias::BeginFrame();

    // The cheat-safe draw modes take in single player only, r_fullbright over r_lightmap.
    s_fullbrightMode = false;
    s_lightmapMode   = false;
    if (cl.maxclients == 1)
    {
        if (r_fullbright.value != 0.0f || cl.worldmodel->lightdata == nullptr)
        {
            s_fullbrightMode = true;
        }
        else if (r_lightmap.value != 0.0f)
        {
            s_lightmapMode = true;
        }
    }
    s_drawLightmaps = !s_fullbrightMode && (cl.worldmodel->lightdata != nullptr);
}

// r_wateralpha and its kin set the map's liquid opacity directly when changed, as in QuakeSpasm.
void SetWaterAlpha(cvar_t * var) { map_wateralpha = var->value; }
void SetLavaAlpha(cvar_t * var)  { map_lavaalpha  = var->value; }
void SetTeleAlpha(cvar_t * var)  { map_telealpha  = var->value; }
void SetSlimeAlpha(cvar_t * var) { map_slimealpha = var->value; }

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    Cvar_RegisterVariable(&r_norefresh);
    Cvar_RegisterVariable(&r_drawentities);
    Cvar_RegisterVariable(&r_drawworld);
    Cvar_RegisterVariable(&r_fullbright);
    Cvar_RegisterVariable(&r_lightmap);
    Cvar_RegisterVariable(&r_wateralpha);
    Cvar_RegisterVariable(&r_lavaalpha);
    Cvar_RegisterVariable(&r_telealpha);
    Cvar_RegisterVariable(&r_slimealpha);
    Cvar_RegisterVariable(&r_dynamic);
    Cvar_RegisterVariable(&r_novis);
    Cvar_RegisterVariable(&r_oldskyleaf);
    Cvar_RegisterVariable(&r_waterwarp);
    Cvar_RegisterVariable(&gl_fullbrights);
    Cvar_RegisterVariable(&gl_farclip);
    Cvar_RegisterVariable(&s_mipFilter);
    Cvar_RegisterVariable(&s_mipBias);
    Cvar_SetCallback(&s_mipFilter, MipFilterChanged);

    Cvar_SetCallback(&r_wateralpha, SetWaterAlpha);
    Cvar_SetCallback(&r_lavaalpha,  SetLavaAlpha);
    Cvar_SetCallback(&r_telealpha,  SetTeleAlpha);
    Cvar_SetCallback(&r_slimealpha, SetSlimeAlpha);
}

// gl_rmisc.c's R_ParseWorldspawn: a map can set its liquids' opacity in worldspawn.
void NewMap()
{
    map_wateralpha = r_wateralpha.value;
    map_lavaalpha  = r_lavaalpha.value;
    map_telealpha  = r_telealpha.value;
    map_slimealpha = r_slimealpha.value;

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

        const float value = static_cast<float>(std::atof(com_token));
        if      (std::strcmp(key, "wateralpha") == 0) { map_wateralpha = value; }
        else if (std::strcmp(key, "lavaalpha")  == 0) { map_lavaalpha  = value; }
        else if (std::strcmp(key, "telealpha")  == 0) { map_telealpha  = value; }
        else if (std::strcmp(key, "slimealpha") == 0) { map_slimealpha = value; }
    }
}

void RenderView()
{
    if (r_norefresh.value != 0.0f)
    {
        return;
    }
    if (cl.worldmodel == nullptr)
    {
        Sys_Error("R_RenderView: NULL worldmodel");
    }

    PS2_PROFILE_SCOPED_EVENT(prof_evt::View);

    SetupView();

    // One stream for every pass of the frame; each submits before the next one starts.
    auto stream = rs::Begin<rs::TriangleStream>(kBatchMaxVerts);

    // The sky first: its layers go under everything, and its surfaces' depth ahead of the world.
    sky::Draw(stream);

    if (r_drawworld.value != 0.0f)
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::World);
        DrawTextureChains(stream, *cl.worldmodel, nullptr, chain_world, s_viewProj);
    }

    DrawEntitiesOnList(stream, false);

    if (r_drawworld.value != 0.0f)
    {
        DrawWaterChains(stream, *cl.worldmodel, nullptr, chain_world, s_viewProj);
    }

    DrawEntitiesOnList(stream, true);

    RenderDlights(stream);
    rs::Submit(stream);

    particles::Draw();
    DrawViewModel();
}

const math::Mat4 & ViewProjection()
{
    return s_viewProj;
}

bool CullModelForEntity(const entity_t & e)
{
    return CullModelBounds(e);
}

math::Mat4 EntityMatrix(const vec3_t origin, const vec3_t angles, const u8 scale)
{
    const float s = ENTSCALE_DECODE(scale);

    math::Mat4 m = math::RotationX(math::DegToRad(angles[ROLL])) *
                   math::RotationY(math::DegToRad(-angles[PITCH])) *
                   math::RotationZ(math::DegToRad(angles[YAW]));

    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            m.m[row][col] *= s;
        }
    }
    m.m[3][0] = origin[0];
    m.m[3][1] = origin[1];
    m.m[3][2] = origin[2];
    m.m[3][3] = 1.0f;
    return m;
}

bool FullbrightMode()
{
    return s_fullbrightMode;
}

bool LightmapMode()
{
    return s_lightmapMode;
}

} // namespace ps2::view

/* ================================================================================================
 * File: alias.cpp
 * Brief: Alias models: QuakeSpasm's r_alias.c on the VU1 path. See alias.h.
 *
 *  A pose is a list of byte-quantized vertices (trivertx_t), and QuakeSpasm draws an alias model
 *  lerped between two of them: scale_origin + scale * (pose1 * (1 - blend) + pose2 * blend). Here
 *  that lerp runs on VU1, on the keyframe format of textured_triangles.vcl. The EE gathers each
 *  corner's two pose words verbatim into the command buffer and the microprogram converts, lerps
 *  and transforms them, with 'scale_origin' folded into the matrix's last row and the two scales
 *  riding with the draw. What else a corner needs - which pose vertex it is, its skin coordinates
 *  - is the same every frame, so it is built once per model, when the model first loads
 *  (GL_MakeAliasModelDisplayLists), and the DMA reads it where it lies.
 *
 *  The shading is QuakeSpasm's: the light at the entity's origin, from the lightmaps under it and
 *  the dynamic lights around it, times a term from the vertex normal and the model's yaw
 *  (anorm_dots.h), lerped between the two poses' normals as the position is. The EE packs that
 *  term into the spare byte of one of the corner's pose words, quantized, and the microprogram
 *  multiplies the entity's light by it.
 *
 *  The skin's fullbright texels then go over the lit skin, as gl_fullbrights adds them: the same
 *  corners drawn again with the glow texture, straight out of the command buffer they are in.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/alias.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/vu1.h"
#include "ps2/math/vec_mat.h"

#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {

// gl_rlight.c's: what R_LightPoint found, and where its trace hit the ground. No header declares
// them; r_alias.c declared them for itself too.
extern vec3_t lightcolor;
extern vec3_t lightspot;

// gl_screen.c's, and the refresh's.
extern cvar_t scr_fov, cl_gun_fovscale;
extern cvar_t r_lerpmodels, r_lerpmove, gl_fullbrights;

// ------------------------------------------------------------------------------------------------
// QuakeSpasm's alias model cvars (gl_rmain.c's) and player skins (r_alias.c's)
// ------------------------------------------------------------------------------------------------

cvar_t r_drawviewmodel      = ps2::MakeCvar("r_drawviewmodel",      "1", CVAR_NONE);
cvar_t r_shadows            = ps2::MakeCvar("r_shadows",            "0", CVAR_ARCHIVE);
cvar_t gl_overbright_models = ps2::MakeCvar("gl_overbright_models", "1", CVAR_ARCHIVE);
cvar_t gl_nocolors          = ps2::MakeCvar("gl_nocolors",          "0", CVAR_NONE);

// The players' skins in their shirt and pants colours, one per scoreboard slot. texmgr.cpp clears
// an entry whose texture it frees.
gltexture_t * playertextures[MAX_SCOREBOARD];

} // extern "C"

namespace ps2::alias {
namespace {

// ------------------------------------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------------------------------------

// Vertices one model's gather may hold before the stream flushes. A model that fits goes out in one
// batch, which the fullbright pass and the shadow then draw again for nothing but a set of chunk
// tags; the largest of id's models, the boss at 555 triangles, fits with room to spare.
constexpr int kLerpBatchMaxVerts = 3 * 768;

// The shade term's table: SHADEDOT_QUANT rows, one per sixteenth of a turn of the model's yaw.
constexpr int kShadeDotQuant = 16;

// Where the light normal index sits in a pose word: trivertx_t's fourth byte, the top one.
constexpr u32 kNormalShift = 24;

// QuakeSpasm's shadow (GL_DrawAliasShadow's SHADOW_*): the model squashed flat, skewed along x as
// GLQuake's were, a tenth of a unit above the ground under it.
constexpr float kShadowSkewX  = -0.7f;
constexpr float kShadowSkewY  = 0.0f;
constexpr float kShadowVScale = 0.0f;
constexpr float kShadowHeight = 0.1f;

// ------------------------------------------------------------------------------------------------
// Shading tables
// ------------------------------------------------------------------------------------------------

// QuakeSpasm's r_avertexnormal_dots: for each of 16 quantized yaws, each normal's shade, which runs
// from 0.7 to 1.99. Kept as id wrote it, with unsuffixed doubles - hence the waiver.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-conversion"
static const float s_shadeDots[kShadeDotQuant][256] = {
    #include "quake/anorm_dots.h"
};
#pragma GCC diagnostic pop

// The same rows quantized to the byte the microprogram reads: shade * 128, which the range above
// keeps inside a byte. Rounded, so the error is half a step either way rather than a darkening.
static u8 s_shadeDotBytes[kShadeDotQuant][256];

// The shade of a model drawn unshaded (r_fullbright, r_lightmap): 1.0 for every normal.
static u8 s_flatShadeBytes[256];

// ------------------------------------------------------------------------------------------------
// Per-model draw data
// ------------------------------------------------------------------------------------------------

// A model's triangles as the draw takes them, three corners each in the file's order: which pose
// vertex a corner is and its skin coordinates - the attribute stream of a keyframe batch.
//
// The same every frame and through every reload of the model, so it is built when the model first
// loads and kept for as long as its qmodel_t is (GLMesh_DeleteVertexBuffers frees the lot). Outside
// the cache, which can move or evict the model between frames, because the DMA reads these where
// they lie, a frame after the draw that recorded them.
struct AliasDraw
{
    vu1::LerpDrawAttrib * corners;    // numCorners of them, qword aligned
    int                   numCorners; // three per triangle
    u32                   bytes;      // the whole block, this header included
    qmodel_t *            owner;
    AliasDraw *           next;       // every model's, for freeing them all
};

static AliasDraw * s_draws = nullptr;

// Builds a model's corners, gl_mesh.c's GL_MakeAliasModelDisplayLists_VBO: a corner on the skin's
// seam takes the coordinates of the skin's back half when its triangle faces away, and every
// coordinate samples a texel's centre.
AliasDraw * BuildAliasDraw(qmodel_t & model, const aliashdr_t & hdr)
{
    const int numCorners  = hdr.numtris * 3;
    const u32 headerBytes = (sizeof(AliasDraw) + 15u) & ~15u;
    const u32 bytes       = headerBytes + (static_cast<u32>(numCorners) * sizeof(vu1::LerpDrawAttrib));

    byte * const block = static_cast<byte *>(
        heap::AllocAligned(heap::MemAlign(16), bytes, heap::MemTag::AliasMdl));

    AliasDraw & draw = *static_cast<AliasDraw *>(static_cast<void *>(block));
    draw.corners    = static_cast<vu1::LerpDrawAttrib *>(static_cast<void *>(block + headerBytes));
    draw.numCorners = numCorners;
    draw.bytes      = bytes;
    draw.owner      = &model;
    draw.next       = s_draws;
    s_draws = &draw;

    const float invWidth  = 1.0f / static_cast<float>(hdr.skinwidth);
    const float invHeight = 1.0f / static_cast<float>(hdr.skinheight);

    vu1::LerpDrawAttrib * out = draw.corners;
    for (int i = 0; i < hdr.numtris; ++i)
    {
        for (int j = 0; j < 3; ++j, ++out)
        {
            const int index = triangles[i].vertindex[j];
            if (index < 0 || index >= hdr.numverts)
            {
                Sys_Error("%s: triangle %d has a bad vertex index (%d)", model.name, i, index);
            }

            int s = stverts[index].s;
            const int t = stverts[index].t;
            if (triangles[i].facesfront == 0 && stverts[index].onseam != 0)
            {
                s += hdr.skinwidth / 2; // on the back side
            }

            out->index = static_cast<u32>(index);
            out->s     = (static_cast<float>(s) + 0.5f) * invWidth;
            out->t     = (static_cast<float>(t) + 0.5f) * invHeight;
            out->q     = 1.0f;
        }
    }
    return &draw;
}

Q_ALWAYS_INLINE const AliasDraw & DrawFor(const qmodel_t & model)
{
    PS2_AssertMsg(model.ps2_render != nullptr, "Alias model without draw data!");
    return *static_cast<const AliasDraw *>(model.ps2_render);
}

// A pose's vertices read as words: one load fetches a whole trivertx_t, position and normal index
// together, the index in the top byte. The poses sit in the model's cache block at an offset that
// is a multiple of 16 from its start, which the cache places 8-byte aligned.
Q_ALWAYS_INLINE const u32 * PoseWords(const aliashdr_t & hdr, const int pose)
{
    static_assert(sizeof(trivertx_t) == sizeof(u32), "trivertx_t must be exactly one word!");

    const byte * const base = static_cast<const byte *>(static_cast<const void *>(&hdr)) + hdr.vertexes;
    return static_cast<const u32 *>(static_cast<const void *>(base)) + (pose * hdr.numverts);
}

// ------------------------------------------------------------------------------------------------
// Frame state
// ------------------------------------------------------------------------------------------------

// The dynamic lights still alive this frame (BeginFrame), so each model's shading tests only those.
static int s_liveDlights[MAX_DLIGHTS];
static int s_numLiveDlights = 0;

// ------------------------------------------------------------------------------------------------
// Pose and transform (r_alias.c's R_SetupAliasFrame, R_SetupEntityTransform)
// ------------------------------------------------------------------------------------------------

// What a model draws at this frame: the two poses and how far it is from the first to the second,
// and where it stands - each lerped from the entity's last two states.
struct LerpData
{
    int    pose1;
    int    pose2;
    float  blend;
    vec3_t origin;
    vec3_t angles;
};

void SetupAliasFrame(entity_t & e, const aliashdr_t & hdr, const float now, LerpData & lerp)
{
    int frame = e.frame;
    if (frame >= hdr.numframes || frame < 0)
    {
        Con_DPrintf("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e.model->name);
        frame = 0;
    }

    int posenum = hdr.frames[frame].firstpose;
    const int numposes = hdr.frames[frame].numposes;

    // A frame group animates on its own, at its interval. A group with none would divide by zero,
    // which QuakeSpasm lets through; it stays on its first pose here.
    if (numposes > 1 && hdr.frames[frame].interval > 0.0f)
    {
        e.lerptime = hdr.frames[frame].interval;
        posenum += static_cast<int>(now / e.lerptime) % numposes;
    }
    else
    {
        e.lerptime = 0.1f;
    }

    if ((e.lerpflags & LERP_RESETANIM) != 0) // kill any lerp in progress
    {
        e.lerpstart    = 0.0f;
        e.previouspose = static_cast<short>(posenum);
        e.currentpose  = static_cast<short>(posenum);
        e.lerpflags    = static_cast<byte>(e.lerpflags & ~LERP_RESETANIM);
    }
    else if (e.currentpose != posenum) // pose changed, start a new lerp
    {
        if ((e.lerpflags & LERP_RESETANIM2) != 0) // defer lerping one more time
        {
            e.lerpstart    = 0.0f;
            e.previouspose = static_cast<short>(posenum);
            e.currentpose  = static_cast<short>(posenum);
            e.lerpflags    = static_cast<byte>(e.lerpflags & ~LERP_RESETANIM2);
        }
        else
        {
            e.lerpstart    = now;
            e.previouspose = e.currentpose;
            e.currentpose  = static_cast<short>(posenum);
        }
    }

    if (r_lerpmodels.value != 0.0f && !((e.model->flags & MOD_NOLERP) != 0 && r_lerpmodels.value != 2.0f))
    {
        float blend;
        if ((e.lerpflags & LERP_FINISH) != 0 && numposes == 1)
        {
            blend = (now - e.lerpstart) / (e.lerpfinish - e.lerpstart);
        }
        else
        {
            blend = (now - e.lerpstart) / e.lerptime;
        }
        lerp.blend = (blend < 0.0f) ? 0.0f : ((blend > 1.0f) ? 1.0f : blend);

        if (lerp.blend == 1.0f)
        {
            e.previouspose = e.currentpose;
        }
        lerp.pose1 = e.previouspose;
        lerp.pose2 = e.currentpose;
    }
    else // don't lerp
    {
        lerp.blend = 1.0f;
        lerp.pose1 = posenum;
        lerp.pose2 = posenum;
    }

    // The entity's poses are its last model's until the client resets them, which a model change
    // asks for (LERP_RESETANIM) but a frame can still slip past: keep them inside this one's.
    if (lerp.pose1 < 0 || lerp.pose1 >= hdr.numposes)
    {
        lerp.pose1 = posenum;
    }
    if (lerp.pose2 < 0 || lerp.pose2 >= hdr.numposes)
    {
        lerp.pose2 = posenum;
    }
}

void SetupEntityTransform(entity_t & e, const float now, LerpData & lerp)
{
    if ((e.lerpflags & LERP_RESETMOVE) != 0) // kill any lerps in progress
    {
        e.movelerpstart = 0.0f;
        VectorCopy(e.origin, e.previousorigin);
        VectorCopy(e.origin, e.currentorigin);
        VectorCopy(e.angles, e.previousangles);
        VectorCopy(e.angles, e.currentangles);
        e.lerpflags = static_cast<byte>(e.lerpflags & ~LERP_RESETMOVE);
    }
    else if (!VectorCompare(e.origin, e.currentorigin) || !VectorCompare(e.angles, e.currentangles))
    {
        // origin/angles changed, start a new lerp
        e.movelerpstart = now;
        VectorCopy(e.currentorigin, e.previousorigin);
        VectorCopy(e.origin, e.currentorigin);
        VectorCopy(e.currentangles, e.previousangles);
        VectorCopy(e.angles, e.currentangles);
    }

    if (r_lerpmove.value != 0.0f && &e != &cl.viewent && (e.lerpflags & LERP_MOVESTEP) != 0)
    {
        float blend;
        if ((e.lerpflags & LERP_FINISH) != 0)
        {
            blend = (now - e.movelerpstart) / (e.lerpfinish - e.movelerpstart);
        }
        else
        {
            blend = (now - e.movelerpstart) / 0.1f;
        }
        blend = (blend < 0.0f) ? 0.0f : ((blend > 1.0f) ? 1.0f : blend);

        vec3_t d;
        VectorSubtract(e.currentorigin, e.previousorigin, d);
        VectorMA(e.previousorigin, blend, d, lerp.origin);

        VectorSubtract(e.currentangles, e.previousangles, d);
        for (int i = 0; i < 3; ++i)
        {
            if (d[i] > 180.0f)  { d[i] -= 360.0f; }
            if (d[i] < -180.0f) { d[i] += 360.0f; }
        }
        VectorMA(e.previousangles, blend, d, lerp.angles);
    }
    else // don't lerp
    {
        VectorCopy(e.origin, lerp.origin);
        VectorCopy(e.angles, lerp.angles);
    }
}

// ------------------------------------------------------------------------------------------------
// Lighting (r_alias.c's R_SetupAliasLighting)
// ------------------------------------------------------------------------------------------------

// Whether the entity is a player's: one of the client's first maxclients entities after the world.
Q_ALWAYS_INLINE bool IsPlayerEntity(const entity_t & e)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(&e);
    return address >= reinterpret_cast<uintptr_t>(&cl_entities[1]) &&
           address <= reinterpret_cast<uintptr_t>(&cl_entities[cl.maxclients]);
}

// Raises a light so its three channels sum to at least 'minimum'.
Q_ALWAYS_INLINE void FloorLight(vec3_t light, const float minimum)
{
    const float add = minimum - (light[0] + light[1] + light[2]);
    if (add > 0.0f)
    {
        light[0] += add / 3.0f;
        light[1] += add / 3.0f;
        light[2] += add / 3.0f;
    }
}

// The entity's light, with QuakeSpasm's floors and clamps, scaled to the 1.0 the shade term
// multiplies (200, as there); and which row of the shade table its yaw picks.
void SetupAliasLighting(entity_t & e, const bool overbright, vec3_t outLight, int * outDotRow)
{
    // If the trace straight down is all black, try again from higher up: this helps with models
    // whose origin is slightly below the ground (some candles in the DOTM start map).
    if (R_LightPoint(e.origin) == 0)
    {
        vec3_t raised;
        VectorCopy(e.origin, raised);
        raised[2] += e.model->maxs[2] * 0.5f;
        R_LightPoint(raised);
    }

    // The dynamic lights, by distance alone.
    for (int i = 0; i < s_numLiveDlights; ++i)
    {
        const dlight_t & dl = cl_dlights[s_liveDlights[i]];

        vec3_t dist;
        VectorSubtract(e.origin, dl.origin, dist);
        const float add = dl.radius - VectorLength(dist);
        if (add > 0.0f)
        {
            lightcolor[0] += add * dl.color[0];
            lightcolor[1] += add * dl.color[1];
            lightcolor[2] += add * dl.color[2];
        }
    }

    if (&e == &cl.viewent)
    {
        FloorLight(lightcolor, 72.0f); // the gun is never darker than 24 a channel
    }
    if (IsPlayerEntity(e))
    {
        FloorLight(lightcolor, 24.0f); // nor a player than 8
    }

    // Clamp so overbright doesn't go too far: 96 a channel.
    if (overbright)
    {
        const float scale = 288.0f / (lightcolor[0] + lightcolor[1] + lightcolor[2]);
        if (scale < 1.0f)
        {
            VectorScale(lightcolor, scale, lightcolor);
        }
    }

    // Hack the brightness up when there are fullbrights but no overbright (256).
    if (gl_fullbrights.value != 0.0f && gl_overbright_models.value == 0.0f &&
        (e.model->flags & MOD_FBRIGHTHACK) != 0)
    {
        lightcolor[0] = lightcolor[1] = lightcolor[2] = 256.0f;
    }

    *outDotRow = static_cast<int>(e.angles[YAW] * (static_cast<float>(kShadeDotQuant) / 360.0f)) & (kShadeDotQuant - 1);
    VectorScale(lightcolor, 1.0f / 200.0f, outLight);
}

// ------------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------------

// The PS2 texture to bind for a skin, or the stand-in checkerboard when there is none.
Q_ALWAYS_INLINE const tex::Texture & BindableTexture(const gltexture_t * gl)
{
    const tex::Texture * const texture = tex::TextureFor(gl);
    return (texture != nullptr) ? *texture : tex::DebugTexture();
}

// Gathers the model's corners into the stream: each corner's two pose words, the second pose's
// verbatim and the first's with its normal index replaced by the corner's shade - the two poses'
// shade terms lerped by 'blend256' (blend in 256ths). See vu1::LerpVertexBytes.
//
// Everything the loop reads it takes by value: the stores go to the command buffer, which under
// -fno-strict-aliasing the compiler must otherwise assume could change what it reads.
void GatherCorners(rs::LerpStream & stream, const AliasDraw & draw, const u32 * const curPose,
                   const u32 * const oldPose, const u8 * const dots, const int blend256)
{
    const vu1::LerpDrawAttrib * src = draw.corners;
    const int numTris = draw.numCorners / 3;

    stream.SetAttribSource(draw.corners);

    for (int t = 0; t < numTris; ++t, src += 3)
    {
        stream.BeginVerts(3);

        // A restrict cursor, so the stores through it don't make the compiler reload everything
        // else after each one (see performance.md). Declared after BeginVerts, which may flush
        // through the stream and so reaches the command buffer too.
        vu1::LerpVertexBytes * const __restrict triPos = stream.PushTriangle();

        for (int i = 0; i < 3; ++i)
        {
            const u32 index   = src[i].index;
            const u32 curBits = curPose[index];
            const u32 oldBits = oldPose[index];

            const int curShade = dots[curBits >> kNormalShift];
            const int oldShade = dots[oldBits >> kNormalShift];
            const u32 shade    = static_cast<u32>(oldShade + (((curShade - oldShade) * blend256) >> 8));

            triPos[i].cur = curBits;
            triPos[i].old = (oldBits & 0x00FFFFFFu) | (shade << kNormalShift);
        }
    }
}

// The model to clip transform the shadow draws under: GL_DrawAliasShadow's, the model rotated,
// squashed onto the ground under it ('groundHeight' below its origin) and skewed, then placed.
math::Mat4 ShadowMatrix(const LerpData & lerp, const float groundHeight)
{
    static const vec3_t kNoOrigin = { 0.0f, 0.0f, 0.0f };

    // In row vectors, the squash between lifting the model by the ground height and lowering it back:
    // x' = x + skewX * (z + h), y' likewise, z' = vscale * (z + h) + height - h.
    const float h = groundHeight;
    const math::Mat4 squash = {{
        { 1.0f,             0.0f,             0.0f,                                      0.0f },
        { 0.0f,             1.0f,             0.0f,                                      0.0f },
        { kShadowSkewX,     kShadowSkewY,     kShadowVScale,                             0.0f },
        { kShadowSkewX * h, kShadowSkewY * h, (kShadowVScale * h) + kShadowHeight - h,   1.0f }
    }};

    const math::Mat4 place = math::Translation(lerp.origin[0], lerp.origin[1], lerp.origin[2]);

    return view::EntityMatrix(kNoOrigin, lerp.angles, ENTSCALE_DEFAULT) * squash * place * view::ViewProjection();
}

// Moves 'scaleOrigin' into the transform's last row: the lerp the microprogram runs produces the
// pose scaled but not offset, so the offset goes in ahead of the rest of the transform.
Q_ALWAYS_INLINE void FoldOffset(math::Mat4 & mvp, const math::Vec3 & scaleOrigin)
{
    const math::Vec4 row3 = math::Transform(math::Vec4{ scaleOrigin.x, scaleOrigin.y, scaleOrigin.z, 1.0f }, mvp);
    mvp.m[3][0] = row3.x;
    mvp.m[3][1] = row3.y;
    mvp.m[3][2] = row3.z;
    mvp.m[3][3] = row3.w;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void Init()
{
    Cvar_RegisterVariable(&r_drawviewmodel);
    Cvar_RegisterVariable(&r_shadows);
    Cvar_RegisterVariable(&gl_overbright_models);
    Cvar_RegisterVariable(&gl_nocolors);

    for (int row = 0; row < kShadeDotQuant; ++row)
    {
        for (int n = 0; n < 256; ++n)
        {
            const float scaled = s_shadeDots[row][n] * 128.0f;
            PS2_Assert(scaled >= 0.0f && scaled <= 255.0f);
            s_shadeDotBytes[row][n] = static_cast<u8>(scaled + 0.5f);
        }
    }
    std::memset(s_flatShadeBytes, 128, sizeof(s_flatShadeBytes));
}

void BeginFrame()
{
    const float now = static_cast<float>(cl.time);

    s_numLiveDlights = 0;
    for (int i = 0; i < MAX_DLIGHTS; ++i)
    {
        if (cl_dlights[i].die >= now)
        {
            s_liveDlights[s_numLiveDlights++] = i;
        }
    }
}

void DrawAliasModel(entity_t & e, const bool viewModel)
{
    // Pose and transform first, so a model that is culled still keeps its lerp state current.
    const aliashdr_t & hdr = *static_cast<const aliashdr_t *>(Mod_Extradata(e.model));
    const AliasDraw & draw = DrawFor(*e.model);

    // QuakeSpasm keeps these times in float too.
    const float now = static_cast<float>(cl.time);

    LerpData lerp;
    SetupAliasFrame(e, hdr, now, lerp);
    SetupEntityTransform(e, now, lerp);

    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntCull);
        if (view::CullModelForEntity(e))
        {
            return;
        }
    }

    // In the cheat-safe draw modes the model draws unshaded: as it is with r_fullbright, flat white
    // with r_lightmap, which also takes no alpha.
    const bool fullbrightMode = view::FullbrightMode();
    const bool lightmapMode   = view::LightmapMode();
    const bool overbright     = (gl_overbright_models.value != 0.0f);

    const float alpha = lightmapMode ? 1.0f : ENTALPHA_DECODE(e.alpha);
    if (alpha == 0.0f)
    {
        return;
    }

    vec3_t light;
    int dotRow;
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntShade);
        SetupAliasLighting(e, overbright, light, &dotRow);
    }

    // The skin: its frame of a skin group, ten a second; a player's in their colours.
    int skinnum = e.skinnum;
    if (skinnum >= hdr.numskins || skinnum < 0)
    {
        Con_DPrintf("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e.model->name);
        skinnum = 0; // skin 0, for WinQuake compatibility
    }
    const int anim = static_cast<int>(now * 10.0f) & 3;

    const gltexture_t * skin = hdr.gltextures[skinnum][anim];
    const gltexture_t * glow = (gl_fullbrights.value != 0.0f) ? hdr.fbtextures[skinnum][anim] : nullptr;
    if (e.colormap != vid.colormap && gl_nocolors.value == 0.0f && IsPlayerEntity(e))
    {
        const gltexture_t * const colored = playertextures[&e - cl_entities - 1];
        if (colored != nullptr)
        {
            skin = colored;
        }
    }
    const tex::Texture & skinTexture = BindableTexture(skin);

    // The view weapon gets QuakeSpasm's cl_gun_fovscale: past 90 degrees of field of view, its y and
    // z are stretched with it, so it doesn't shrink into the corner of the screen.
    float fovScale = 1.0f;
    if (&e == &cl.viewent && scr_fov.value > 90.0f && cl_gun_fovscale.value != 0.0f)
    {
        fovScale = std::tan(scr_fov.value * (0.5f * math::kPI / 180.0f));
    }

    const math::Vec3 scaleOrigin = { hdr.scale_origin[0], hdr.scale_origin[1] * fovScale, hdr.scale_origin[2] * fovScale };
    const math::Vec3 scale       = { hdr.scale[0],        hdr.scale[1] * fovScale,        hdr.scale[2] * fovScale };

    // The second pose is the microprogram's current keyframe, the first its old one.
    const math::Vec3 frontv = { scale.x * lerp.blend, scale.y * lerp.blend, scale.z * lerp.blend };
    const float backLerp = 1.0f - lerp.blend;
    const math::Vec3 backv  = { scale.x * backLerp, scale.y * backLerp, scale.z * backLerp };

    // The model's light, in the units the microprogram multiplies the quantized shade term by, and
    // the vertex alpha the draw blends at. QuakeSpasm doubles the lit colour for gl_overbright_models;
    // MODULATE's 128 is the texel unchanged, so the light's 1.0 lands on it. An RGBA skin's texels
    // carry alpha 0xFF, twice the GS's 1.0, so its vertex alpha is halved to blend at the same opacity.
    const float alphaScale = (skinTexture.components == tex::TexComponents::RGBA) ? 64.0f : 128.0f;
    math::Vec4 shadeLight;
    const u8 * dots;
    if (lightmapMode)
    {
        shadeLight = { 255.0f / 128.0f, 255.0f / 128.0f, 255.0f / 128.0f, 128.0f };
        dots = s_flatShadeBytes;
    }
    else if (fullbrightMode)
    {
        shadeLight = { 1.0f, 1.0f, 1.0f, alpha * alphaScale };
        dots = s_flatShadeBytes;
    }
    else
    {
        const float lightScale = overbright ? 2.0f : 1.0f;
        shadeLight = { light[0] * lightScale, light[1] * lightScale, light[2] * lightScale, alpha * alphaScale };
        dots = s_shadeDotBytes[dotRow];
    }

    rs::DrawFlags flags = (alpha < 1.0f) ? rs::DrawFlags::Blended : rs::DrawFlags::None;
    if (lightmapMode)
    {
        flags = flags | rs::DrawFlags::Untextured;
    }

    // The view weapon's depth goes to the near end of the z-buffer, QuakeSpasm's glDepthRange(0, 0.3),
    // so it never pokes into a wall it is held against.
    const rs::DrawFlags depthFlags = viewModel ? rs::DrawFlags::DepthHack : rs::DrawFlags::None;

    // R_RotateForEntity, with the pose's offset folded in.
    math::Mat4 mvp = view::EntityMatrix(lerp.origin, lerp.angles, e.scale) * view::ViewProjection();
    FoldOffset(mvp, scaleOrigin);

    const u32 * const curPose = PoseWords(hdr, lerp.pose2);
    const u32 * const oldPose = PoseWords(hdr, lerp.pose1);
    const int blend256 = static_cast<int>(lerp.blend * 256.0f);

    // A model that fits one batch leaves all of its corners in the command buffer, for the passes
    // after this one to draw again.
    const bool onePass = (draw.numCorners <= kLerpBatchMaxVerts);

    auto stream = rs::Begin<rs::LerpStream>(kLerpBatchMaxVerts);

    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntGeom);

        stream.SetTransform(mvp);
        stream.SetTexture(skinTexture);
        stream.SetDrawFlags(flags | depthFlags);
        stream.SetFaceCull(rs::FaceCull::Negative);
        stream.SetLerpParams(frontv, backv, shadeLight);
        GatherCorners(stream, draw, curPose, oldPose, dots, blend256);
        rs::Submit(stream);

        // The fullbright texels, added over the lit skin: their glow texture draws its texels as
        // they are (TexFunction::Decal), so the light and shade the corners carry don't reach them,
        // at the vertex alpha - the entity's - and its other texels are black.
        const tex::Texture * const glowTexture = tex::TextureFor(glow);
        if (glowTexture != nullptr && !lightmapMode)
        {
            stream.SetTexture(*glowTexture);
            stream.SetDrawFlags(rs::DrawFlags::Additive | depthFlags);
            stream.SetLerpParams(frontv, backv, { 0.0f, 0.0f, 0.0f, alpha * 128.0f });
            if (onePass)
            {
                rs::Resubmit(stream);
            }
            else
            {
                GatherCorners(stream, draw, curPose, oldPose, dots, blend256);
                rs::Submit(stream);
            }
        }
    }

    // The shadow, with r_shadows: the same corners again, squashed onto the ground under the model,
    // black at half the entity's alpha. QuakeSpasm draws every shadow before the entities, and keeps
    // a stencil so overlapping triangles darken once; here each follows its model, and the backface
    // cull alone keeps most of the folds from darkening twice.
    if (r_shadows.value != 0.0f && !viewModel && !lightmapMode && (e.model->flags & MOD_NOSHADOW) == 0)
    {
        PS2_PROFILE_SCOPED_EVENT(prof_evt::EntShadow);

        R_LightPoint(e.origin);
        math::Mat4 shadowMvp = ShadowMatrix(lerp, e.origin[2] - lightspot[2]);
        FoldOffset(shadowMvp, { hdr.scale_origin[0], hdr.scale_origin[1], hdr.scale_origin[2] });

        const math::Vec3 shadowFrontv = { hdr.scale[0] * lerp.blend, hdr.scale[1] * lerp.blend, hdr.scale[2] * lerp.blend };
        const math::Vec3 shadowBackv  = { hdr.scale[0] * backLerp,   hdr.scale[1] * backLerp,   hdr.scale[2] * backLerp };

        stream.SetTransform(shadowMvp);
        stream.SetTexture(skinTexture);
        stream.SetDrawFlags(rs::DrawFlags::Blended | rs::DrawFlags::Untextured);
        stream.SetLerpParams(shadowFrontv, shadowBackv, { 0.0f, 0.0f, 0.0f, ENTALPHA_DECODE(e.alpha) * 64.0f });
        if (onePass)
        {
            rs::Resubmit(stream);
        }
        else
        {
            GatherCorners(stream, draw, curPose, oldPose, dots, blend256);
            rs::Submit(stream);
        }
    }
}

void NewGame()
{
    for (gltexture_t *& texture : playertextures)
    {
        texture = nullptr;
    }
}

} // namespace ps2::alias

extern "C" {

// ------------------------------------------------------------------------------------------------
// Load-time hooks gl_model.c calls
// ------------------------------------------------------------------------------------------------

// Called by Mod_LoadAliasModel each time the model loads, its working arrays (stverts, triangles,
// poseverts) still holding it: gl_mesh.c's, building what the draw reads.
void GL_MakeAliasModelDisplayLists(qmodel_t * m, aliashdr_t * hdr)
{
    // The poses, each vertex in the file's order, where the draw gathers its pose words from: on the
    // hunk, with the rest of the model, which moves into the cache when the load is done and is read
    // in again after an eviction. QuakeSpasm's VBO path keeps them the same way, in 'vertexes'.
    const int poseBytes = hdr->numverts * static_cast<int>(sizeof(trivertx_t));
    byte * const poses = static_cast<byte *>(Hunk_Alloc(hdr->numposes * poseBytes));
    for (int i = 0; i < hdr->numposes; ++i)
    {
        std::memcpy(poses + (i * poseBytes), poseverts[i], static_cast<size_t>(poseBytes));
    }
    hdr->vertexes  = static_cast<intptr_t>(poses - static_cast<byte *>(static_cast<void *>(hdr)));
    hdr->poseverts = hdr->numverts;

    // The corners only the first time: they are the same for every load of the same file.
    if (m->ps2_render == nullptr)
    {
        m->ps2_render = ps2::alias::BuildAliasDraw(*m, *hdr);
    }
}

// Mod_ResetAll's, ahead of clearing every qmodel_t: the corners of every alias model go.
void GLMesh_DeleteVertexBuffers()
{
    using ps2::alias::s_draws;

    if (s_draws != nullptr)
    {
        // The frame the GS may still be drawing can be reading them.
        ps2::rs::FinishFrameInFlight();
    }
    while (s_draws != nullptr)
    {
        ps2::alias::AliasDraw * const draw = s_draws;
        s_draws = draw->next;
        draw->owner->ps2_render = nullptr;
        ps2::heap::Free(draw, draw->bytes, ps2::heap::MemTag::AliasMdl);
    }
}

// ------------------------------------------------------------------------------------------------
// Player skins (gl_rmisc.c's)
// ------------------------------------------------------------------------------------------------

// New colours for a player: their skin texture translated again.
void R_TranslatePlayerSkin(int playernum)
{
    const int top    = (cl.scores[playernum].colors & 0xf0) >> 4;
    const int bottom = cl.scores[playernum].colors & 15;

    if (gl_nocolors.value == 0.0f && playertextures[playernum] != nullptr)
    {
        TexMgr_ReloadImage(playertextures[playernum], top, bottom);
    }
}

// A new skin or model for a player: their texture made again from the skin's indices, then
// translated. Called when the skin or the model actually changes, not just the colours.
void R_TranslateNewPlayerSkin(int playernum)
{
    entity_t & e = cl_entities[1 + playernum];
    if (e.model == nullptr || e.model->type != mod_alias)
    {
        return;
    }

    aliashdr_t * const hdr = static_cast<aliashdr_t *>(Mod_Extradata(e.model));

    int skinnum = e.skinnum;
    if (skinnum < 0 || skinnum >= hdr->numskins)
    {
        Con_DPrintf("(%d): Invalid player skin #%d\n", playernum, skinnum);
        skinnum = 0;
    }

    // The 8-bit skin the model loader kept for this, in the model's cache block: read at once.
    byte * const pixels = static_cast<byte *>(static_cast<void *>(hdr)) + hdr->texels[skinnum];

    char name[64];
    q_snprintf(name, sizeof(name), "player_%i", playernum);
    playertextures[playernum] = TexMgr_LoadImage(e.model, name, hdr->skinwidth, hdr->skinheight, SRC_INDEXED,
                                                 pixels, hdr->gltextures[skinnum][0]->source_file,
                                                 hdr->gltextures[skinnum][0]->source_offset,
                                                 TEXPREF_PAD | TEXPREF_OVERWRITE);

    R_TranslatePlayerSkin(playernum);
}

} // extern "C"

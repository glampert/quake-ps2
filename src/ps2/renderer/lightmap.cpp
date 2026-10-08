/* ================================================================================================
 * File: lightmap.cpp
 * Brief: The lightmap atlases and their rebuilds. See lightmap.h.
 *
 *  QuakeSpasm's r_brush.c lightmap code with gl_overbright on, one channel wide: a luxel is the
 *  sum of its samples, each scaled by its style's 8.8 brightness (d_lightstylevalue, from
 *  R_AnimateLight), plus whatever dynamic lights reach it, stored shifted down by 8 - so 128 is
 *  the texture's own colour and 255 almost twice it, which the GS's Cd * As / 128 blend reaches
 *  through the light ramp.
 *
 *  Atlases are packed with the skyline packer the 2D scrap atlases use, a new one opened when
 *  the current one is full, as QuakeSpasm's AllocBlock does. A rebuilt surface re-uploads its
 *  whole atlas, where QuakeSpasm uploads the rows that changed.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/lightmap.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/scrap_atlas.h" // SkylinePacker, shared with the 2D scrap atlases
#include "ps2/math/math.h"

#include <cstdio>
#include <cstring>

namespace ps2::lm {
namespace {

// ------------------------------------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------------------------------------

// Largest surface the accumulator takes, in luxels. gl_model.c lets a surface's extents reach
// 2000 units (126 luxels) where id's limit was 256 (17); id's maps keep to the old one. So this
// is room for 64x64, four times what any id map needs, and a bigger surface is a Sys_Error
// rather than an accumulator the size of QuakeSpasm's (an atlas's worth, 768 KB of .bss).
constexpr int kMaxSurfaceLuxels = 64 * 64;

constexpr int kAtlasLuxels = kAtlasWidth * kAtlasHeight;

// Luxels a surface spans, from its texture-space extents.
Q_ALWAYS_INLINE int LuxelsWide(const msurface_t & surf) { return (surf.extents[0] >> 4) + 1; }
Q_ALWAYS_INLINE int LuxelsHigh(const msurface_t & surf) { return (surf.extents[1] >> 4) + 1; }

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------

// The atlases: the texture the GS samples and the luxels it is made of. The pixels are heap blocks,
// so a map only pays for the atlases it fills.
static tex::Texture * s_atlases[kMaxAtlases] = {};
static u8 *           s_pixels[kMaxAtlases]  = {};
static int            s_numAtlases = 0;

// Packs each surface's block into the atlas being built.
static SkylinePacker<kAtlasWidth, kAtlasHeight> s_packer;
static bool s_building = false;

// Where a surface's luxels are summed before they are shifted, clamped and stored: QuakeSpasm's
// blocklights, one channel wide.
static u32 s_blockLights[kMaxSurfaceLuxels];

// ------------------------------------------------------------------------------------------------
// Atlas lifetime
// ------------------------------------------------------------------------------------------------

void NewAtlas()
{
    if (s_numAtlases == kMaxAtlases) [[unlikely]]
    {
        Sys_Error("Out of lightmap atlases! Bump lm::kMaxAtlases (%d).", kMaxAtlases);
    }

    const int index = s_numAtlases++;

    // Cache-line aligned, as the upload DMA reads it straight out of EE RAM: a dirty line of
    // something else's could otherwise be written back over it. Filled at full light, so a UV
    // straying a luxel outside its block reads as lit rather than as a black seam.
    u8 * const pixels = static_cast<u8 *>(
        ps2::heap::AllocAligned(ps2::heap::MemAlign(64), kAtlasLuxels, ps2::heap::MemTag::Lightmap));
    std::memset(pixels, 128, kAtlasLuxels);

    char name[16];
    std::snprintf(name, sizeof(name), "*lightmap%d", index);

    // Linear, or the 16-unit luxels would tile; the alpha is the light, so it must reach the
    // texture function.
    tex::Texture & atlas = tex::Create(name, pixels, kAtlasWidth, kAtlasHeight, tex::PixelFormat::Light8,
                                       tex::TexComponents::RGBA, tex::ImageType::Wall);
    atlas.magFilter = tex::TexFilter::Linear;
    atlas.minFilter = tex::TexFilter::Linear;

    s_atlases[index] = &atlas;
    s_pixels[index]  = pixels;

    s_packer.Reset();
}

// ------------------------------------------------------------------------------------------------
// Luxel accumulation (QuakeSpasm's R_BuildLightMap / R_AddDynamicLights)
// ------------------------------------------------------------------------------------------------

// Adds the dynamic lights R_MarkLights flagged on the surface to its summed luxels, each falling
// off linearly over an octagonal distance - max + min/2, no square root. QuakeSpasm's, with the
// light's colour averaged into the one channel; Quake's own lights are white.
void AddDynamicLights(const msurface_t & surf)
{
    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);
    const mtexinfo_t & tex = *surf.texinfo;

    for (int lnum = 0; lnum < MAX_DLIGHTS; ++lnum)
    {
        if ((surf.dlightbits[lnum >> 5] & (1u << (lnum & 31))) == 0)
        {
            continue; // Not lit by this one.
        }

        const dlight_t & dl = cl_dlights[lnum];

        // How much of the light's radius survives the trip to the surface's plane.
        const float planeDist = DotProduct(dl.origin, surf.plane->normal) - surf.plane->dist;
        const float rad = dl.radius - math::Fabsf(planeDist);
        if (rad < dl.minlight)
        {
            continue;
        }
        const float minlight = rad - dl.minlight;

        // The light projected onto the plane, then into the surface's luxel grid.
        vec3_t impact;
        for (int i = 0; i < 3; ++i)
        {
            impact[i] = dl.origin[i] - (surf.plane->normal[i] * planeDist);
        }
        const float local[2] = {
            DotProduct(impact, tex.vecs[0]) + tex.vecs[0][3] - static_cast<float>(surf.texturemins[0]),
            DotProduct(impact, tex.vecs[1]) + tex.vecs[1][3] - static_cast<float>(surf.texturemins[1])
        };

        const float scale = (dl.color[0] + dl.color[1] + dl.color[2]) * (256.0f / 3.0f);

        u32 * bl = s_blockLights;
        for (int t = 0; t < tmax; ++t)
        {
            int td = static_cast<int>(local[1] - static_cast<float>(t * kLuxelSizeUnits));
            td = (td < 0) ? -td : td;

            for (int s = 0; s < smax; ++s, ++bl)
            {
                int sd = static_cast<int>(local[0] - static_cast<float>(s * kLuxelSizeUnits));
                sd = (sd < 0) ? -sd : sd;

                const float dist = static_cast<float>((sd > td) ? (sd + (td >> 1)) : (td + (sd >> 1)));
                if (dist < minlight)
                {
                    *bl += static_cast<u32>((rad - dist) * scale);
                }
            }
        }
    }
}

// Sums the surface's luxels and stores them into its atlas block, recording the style brightness
// they were built at - what UpdateSurface compares against to tell when they are stale.
void BuildLightmap(msurface_t & surf)
{
    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);
    const int size = smax * tmax;

    if (size > kMaxSurfaceLuxels) [[unlikely]]
    {
        Sys_Error("Surface lightmap of %dx%d luxels is over the %d the accumulator takes.",
                  smax, tmax, kMaxSurfaceLuxels);
    }

    surf.cached_dlight = (surf.dlightframe == r_framecount);

    if (cl.worldmodel->lightdata != nullptr)
    {
        std::memset(s_blockLights, 0, static_cast<size_t>(size) * sizeof(s_blockLights[0]));

        const byte * samples = surf.samples;
        if (samples != nullptr)
        {
            for (int map = 0; map < MAXLIGHTMAPS && surf.styles[map] != 255; ++map)
            {
                const u32 scale = static_cast<u32>(d_lightstylevalue[surf.styles[map]]);
                surf.cached_light[map] = static_cast<int>(scale); // 8.8 fraction

                for (int i = 0; i < size; ++i)
                {
                    s_blockLights[i] += samples[i] * scale;
                }
                samples += size; // Next style's map.
            }
        }

        if (surf.dlightframe == r_framecount)
        {
            AddDynamicLights(surf);
        }
    }
    else
    {
        // A map built without light data draws fully lit, as in QuakeSpasm.
        for (int i = 0; i < size; ++i)
        {
            s_blockLights[i] = 255u << 8;
        }
    }

    // Shifted down to the light over two (gl_overbright), clamped, and floored at 1: the light ramp
    // maps 0 to alpha 0, which the alpha test drops, and a dropped luxel would leave the wall
    // under it unlit rather than black.
    u8 * dst = s_pixels[surf.lightmaptexturenum] + (surf.light_t * kAtlasWidth) + surf.light_s;
    const u32 * bl = s_blockLights;
    for (int t = 0; t < tmax; ++t, dst += kAtlasWidth)
    {
        for (int s = 0; s < smax; ++s, ++bl)
        {
            const u32 light = *bl >> 8;
            dst[s] = static_cast<u8>((light > 255u) ? 255u : ((light < 1u) ? 1u : light));
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

void ReleaseAtlases()
{
    for (int i = 0; i < s_numAtlases; ++i)
    {
        tex::Destroy(*s_atlases[i]);
        ps2::heap::Free(s_pixels[i], kAtlasLuxels, ps2::heap::MemTag::Lightmap);
        s_atlases[i] = nullptr;
        s_pixels[i]  = nullptr;
    }
    s_numAtlases = 0;
}

void BeginBuilding()
{
    ReleaseAtlases();
    NewAtlas();
    s_building = true;
}

void CreateSurfaceLightmap(msurface_t & surf)
{
    PS2_AssertMsg(s_building, "CreateSurfaceLightmap outside BeginBuilding/EndBuilding!");
    PS2_Assert((surf.flags & SURF_DRAWTILED) == 0);

    const int smax = LuxelsWide(surf);
    const int tmax = LuxelsHigh(surf);

    int lightS = 0;
    int lightT = 0;
    if (!s_packer.Alloc(smax, tmax, &lightS, &lightT))
    {
        // Atlas full: start a fresh one and try once more. A block that will not fit an empty
        // atlas is a broken map, not a packing failure.
        NewAtlas();
        if (!s_packer.Alloc(smax, tmax, &lightS, &lightT)) [[unlikely]]
        {
            Sys_Error("Lightmap block of %dx%d luxels does not fit a %dx%d atlas.",
                      smax, tmax, kAtlasWidth, kAtlasHeight);
        }
    }

    surf.lightmaptexturenum = s_numAtlases - 1;
    surf.light_s = lightS;
    surf.light_t = lightT;

    BuildLightmap(surf);
}

void EndBuilding()
{
    s_building = false;
    Con_DPrintf("Lightmaps: %d atlas(es) of %dx%d, %d KB.\n",
                s_numAtlases, kAtlasWidth, kAtlasHeight, (s_numAtlases * kAtlasLuxels) / 1024);
}

void UpdateSurface(msurface_t & surf)
{
    PS2_Assert((surf.flags & SURF_DRAWTILED) == 0);

    bool stale = (surf.dlightframe == r_framecount) || surf.cached_dlight;
    for (int map = 0; !stale && map < MAXLIGHTMAPS && surf.styles[map] != 255; ++map)
    {
        stale = (d_lightstylevalue[surf.styles[map]] != surf.cached_light[map]);
    }

    if (stale && r_dynamic.value != 0.0f)
    {
        BuildLightmap(surf);
        s_atlases[surf.lightmaptexturenum]->MarkPixelsDirty();
    }
}

int NumAtlases()
{
    return s_numAtlases;
}

const tex::Texture & AtlasTexture(const int index)
{
    PS2_Assert(index >= 0 && index < s_numAtlases);
    return *s_atlases[index];
}

} // namespace ps2::lm

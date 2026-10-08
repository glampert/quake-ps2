/* ================================================================================================
 * File: refresh.cpp
 * Brief: The refresh: QuakeSpasm's render.h seam (R_Init, R_NewMap, R_RenderView, ...), the
 *        renderer state and cvars the client and the kept renderer files read, and the load-time
 *        renderer hooks gl_model.c calls for the sky.
 *        The Quake 1 counterpart of the Quake II port's ref.cpp.
 *
 *        R_Init and R_NewMap are gl_rmisc.c's - cvars, particles, light styles, efrags, fog - with
 *        the brush models' draw data and lightmaps built where GL_BuildLightmaps ran; the view
 *        itself is view.cpp.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/alias.h"
#include "ps2/renderer/brush.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/view.h"

extern "C" {

// ------------------------------------------------------------------------------------------------
// View state, written by the client's view code (view.c) and read by the kept renderer files
// ------------------------------------------------------------------------------------------------

refdef_t r_refdef;
vec3_t   r_origin, vpn, vright, vup;
int      r_framecount;
int      d_lightstylevalue[256]; // 8.8 fraction of base light value, from R_AnimateLight

// The vertex normal table alias models index by lightnormalindex. r_part.c's entity particles
// use it too, which is why it is here rather than with the alias model drawing. The table is
// QuakeSpasm's, written as unsuffixed doubles; rounded to float is exactly what QuakeSpasm
// stores too.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-conversion"
const float r_avertexnormals[NUMVERTEXNORMALS][3] = {
    #include "quake/anorms.h"
};
#pragma GCC diagnostic pop

// ------------------------------------------------------------------------------------------------
// Renderer cvars the engine reads. QuakeSpasm registers them in R_Init, and so does this.
// ------------------------------------------------------------------------------------------------

cvar_t r_lerpmodels      = ps2::MakeCvar("r_lerpmodels",      "1", CVAR_NONE);
cvar_t r_lerpmove        = ps2::MakeCvar("r_lerpmove",        "1", CVAR_NONE);
cvar_t r_flatlightstyles = ps2::MakeCvar("r_flatlightstyles", "0", CVAR_NONE);
cvar_t gl_clear          = ps2::MakeCvar("gl_clear",          "1", CVAR_NONE);
cvar_t gl_polyblend      = ps2::MakeCvar("gl_polyblend",      "1", CVAR_NONE);
cvar_t gl_flashblend     = ps2::MakeCvar("gl_flashblend",     "0", CVAR_ARCHIVE);

// The colour the frame is cleared to, as a palette index: QuakeSpasm's dark grey by default.
cvar_t r_clearcolor = ps2::MakeCvar("r_clearcolor", "2", CVAR_ARCHIVE);

// The models gl_model.c flags as never lerped / never shadowed, by name.
cvar_t r_nolerp_list = ps2::MakeCvar("r_nolerp_list",
    "progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,"
    "progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl",
    CVAR_NONE);

cvar_t r_noshadow_list = ps2::MakeCvar("r_noshadow_list",
    "progs/flame2.mdl,progs/flame.mdl,progs/bolt1.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/laser.mdl",
    CVAR_NONE);

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

// r_clearcolor's callback: QuakeSpasm's R_SetClearColor_f.
static void ClearColorChanged(cvar_t * var)
{
    const u32 rgba = d_8to24table[static_cast<int>(var->value) & 0xFF];
    ps2::rs::SetClearColor(static_cast<u8>(rgba & 0xFFu), static_cast<u8>((rgba >> 8) & 0xFFu),
                           static_cast<u8>((rgba >> 16) & 0xFFu));
}

void R_Init()
{
    Cvar_RegisterVariable(&r_lerpmodels);
    Cvar_RegisterVariable(&r_lerpmove);
    Cvar_RegisterVariable(&r_flatlightstyles);
    Cvar_RegisterVariable(&gl_clear);
    Cvar_RegisterVariable(&gl_polyblend);
    Cvar_RegisterVariable(&gl_flashblend);
    Cvar_RegisterVariable(&r_nolerp_list);
    Cvar_RegisterVariable(&r_noshadow_list);
    Cvar_RegisterVariable(&r_clearcolor);
    Cvar_SetCallback(&r_clearcolor, ClearColorChanged);

    ps2::view::Init();
    ps2::alias::Init();

    R_InitParticles();
    ClearColorChanged(&r_clearcolor);
    Fog_Init();
}

void R_NewMap()
{
    for (int i = 0; i < 256; ++i)
    {
        d_lightstylevalue[i] = 264; // normal light value
    }

    // Clear out efrags in case the level hasn't been reloaded.
    for (int i = 0; i < cl.worldmodel->numleafs; ++i)
    {
        cl.worldmodel->leafs[i].efrags = nullptr;
    }

    r_viewleaf = nullptr;
    R_ClearParticles();

    // GL_BuildLightmaps' "no dlightcache": the surfaces' dlight frames start at 0, so a build at
    // frame 0 would take every one of them for lit by a dynamic light last frame.
    r_framecount = 1;
    ps2::brush::BuildForNewMap();

    r_framecount    = 0;
    r_visframecount = 0;

    Fog_NewMap();          // global fog, from worldspawn
    ps2::view::NewMap();   // the liquids' opacity, from worldspawn
}

// A game switch: the player skin textures go, which TexMgr_NewGame has just freed.
void R_NewGame()
{
    ps2::alias::NewGame();
}

void D_FlushCaches() {}

// ------------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------------

void R_RenderView()
{
    ps2::view::RenderView();
}

// ------------------------------------------------------------------------------------------------
// Load-time hooks the model loader calls
// ------------------------------------------------------------------------------------------------

// gl_warp.c's, which the PS2 doesn't build: QuakeSpasm cuts its water into 128-unit polygons as
// the map loads, for a warp it computes per texel, and renders each warping texture into a warp
// image this size. The PS2 warps per vertex, on its own 32-unit cut (see brush.cpp), and has no
// warp images: gl_model.c still registers the cut's cvar and reads the image size, 0.
cvar_t gl_subdivide_size = ps2::MakeCvar("gl_subdivide_size", "128", CVAR_ARCHIVE);
int    gl_warpimagesize  = 0;

void GL_SubdivideSurface(msurface_t * fa)
{
    (void)fa;
}

// For now only the sky's flat colour, which the view draws sky surfaces in: gl_sky.c's average
// of the opaque texels of the front layer, the left half of the 256x128 image (index 0 is the
// layer's transparent colour), as it works it out for r_fastsky.
void Sky_LoadTexture(qmodel_t * mod, texture_t * mt)
{
    (void)mod;

    const int halfWidth = static_cast<int>(mt->width / 2);
    const int height    = static_cast<int>(mt->height);
    const byte * const src = static_cast<const byte *>(static_cast<const void *>(mt + 1));

    u32 r = 0, g = 0, b = 0, count = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < halfWidth; ++x)
        {
            const byte p = src[(y * static_cast<int>(mt->width)) + x];
            if (p != 0)
            {
                const u32 rgba = d_8to24table[p];
                r += rgba & 0xFFu;
                g += (rgba >> 8) & 0xFFu;
                b += (rgba >> 16) & 0xFFu;
                ++count;
            }
        }
    }

    if (count > 0)
    {
        ps2::view::SetSkyFlatColor(static_cast<u8>(r / count), static_cast<u8>(g / count),
                                   static_cast<u8>(b / count));
    }
}
void Sky_LoadTextureQ64(qmodel_t * mod, texture_t * mt) { (void)mod; (void)mt; }
void Sky_LoadSkyBox(const char * name)                  { (void)name; }
void Sky_ClearAll() {}

// No external replacement textures (textures/*.tga and the like) on the PS2: the textures come
// from the BSP, the WAD and the models.
byte * Image_LoadImage(const char * name, int * width, int * height)
{
    (void)name;
    (void)width;
    (void)height;
    return nullptr;
}

} // extern "C"

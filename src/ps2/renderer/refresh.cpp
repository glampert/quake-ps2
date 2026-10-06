/* ================================================================================================
 * File: refresh.cpp
 * Brief: The refresh: QuakeSpasm's render.h seam (R_Init, R_NewMap, R_RenderView, ...), the
 *        renderer state and cvars the client and the kept renderer files read, and the load-time
 *        renderer hooks gl_model.c calls (sky textures, warp subdivision, alias mesh building).
 *        The Quake 1 counterpart of the Quake II port's ref.cpp.
 *
 *        Nothing is drawn yet, and nothing is built for drawing: R_Init and R_NewMap do only the
 *        engine-visible part of QuakeSpasm's gl_rmisc.c - cvars, particles, light styles, efrags,
 *        fog - and the view renders nothing.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/renderer/render_system.h"

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

// Warp image size, which gl_model.c reads when it loads a turbulent texture. The PS2 warps
// texture coordinates on VU1 instead of rendering warp images, so there are none.
int gl_warpimagesize = 0;

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
    "progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl", CVAR_NONE);
cvar_t r_noshadow_list = ps2::MakeCvar("r_noshadow_list",
    "progs/flame2.mdl,progs/flame.mdl,progs/bolt1.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/laser.mdl",
    CVAR_NONE);

// Registered by gl_model.c's Mod_Init, which runs before R_Init.
cvar_t gl_subdivide_size = ps2::MakeCvar("gl_subdivide_size", "128", CVAR_ARCHIVE);

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

    R_ClearParticles();
    r_framecount = 0;

    Fog_NewMap(); // global fog, from worldspawn
}

// QuakeSpasm's R_NewGame forgets the player skin textures, which TexMgr_NewGame has just freed;
// with no skins built yet there is nothing to forget.
void R_NewGame() {}

void D_FlushCaches() {}

// ------------------------------------------------------------------------------------------------
// Frame: nothing is drawn yet
// ------------------------------------------------------------------------------------------------

void R_RenderView() {}

// engine_hooks.h:
void PS2_DrawPolyBlend(const float rgba[4]) { (void)rgba; }

void R_TranslatePlayerSkin(int playernum)    { (void)playernum; }
void R_TranslateNewPlayerSkin(int playernum) { (void)playernum; }

// ------------------------------------------------------------------------------------------------
// Load-time hooks the model loader calls
// ------------------------------------------------------------------------------------------------

void Sky_LoadTexture(qmodel_t * mod, texture_t * mt)    { (void)mod; (void)mt; }
void Sky_LoadTextureQ64(qmodel_t * mod, texture_t * mt) { (void)mod; (void)mt; }
void Sky_LoadSkyBox(const char * name)                  { (void)name; }
void Sky_ClearAll() {}

void GL_SubdivideSurface(msurface_t * fa) { (void)fa; }
void GL_MakeAliasModelDisplayLists(qmodel_t * m, aliashdr_t * hdr) { (void)m; (void)hdr; }
void GLMesh_DeleteVertexBuffers() {}

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

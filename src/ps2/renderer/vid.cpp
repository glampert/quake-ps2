/* ================================================================================================
 * File: vid.cpp
 * Brief: QuakeSpasm's VID_* video seam and the frame bracket SCR_UpdateScreen draws inside
 *        (GL_BeginRendering/GL_EndRendering). Owns the engine's 'vid' screen state.
 *
 *        No GS yet: this sets up the screen state the client lays its console, status bar and
 *        menus out against, and the frame bracket draws nothing. The GS comes up here when 2D
 *        rendering is ported.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/sys.h"

namespace {

// The PS2 framebuffer: 640x448 fits an NTSC field pair (and PAL's 512 lines with room to spare).
constexpr int kScreenWidth  = 640;
constexpr int kScreenHeight = 448;

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// Engine-visible video state
// ------------------------------------------------------------------------------------------------

viddef_t    vid       = {};
modestate_t modestate = MS_UNINIT;

// No GLSL, so no shader gamma: the options menu hides what depends on it.
qboolean gl_glsl_gamma_able = false;

// The options menu's gamma and contrast sliders, which QuakeSpasm keeps in the video module.
cvar_t vid_gamma    = ps2::MakeCvar("gamma",    "1", CVAR_ARCHIVE);
cvar_t vid_contrast = ps2::MakeCvar("contrast", "1", CVAR_ARCHIVE);

// ------------------------------------------------------------------------------------------------
// VID_*
// ------------------------------------------------------------------------------------------------

void VID_Init()
{
    // The backend's own console commands; see RegisterCommands for why here.
    ps2::sys::RegisterCommands();

    // Host_Init queues "vid_unlock" to run after the configs: QuakeSpasm locks its video mode
    // while they do. With one fixed mode there is nothing to lock or unlock.
    Cmd_AddCommand("vid_unlock", []() {});

    Cvar_RegisterVariable(&vid_gamma);
    Cvar_RegisterVariable(&vid_contrast);

    vid.width    = kScreenWidth;
    vid.height   = kScreenHeight;
    vid.aspect   = static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight);
    vid.numpages = 2;

    // The console's virtual size. SCR_Conwidth_f recomputes it from scr_conwidth and
    // scr_conscale as soon as a config sets either.
    vid.conwidth  = kScreenWidth;
    vid.conheight = kScreenHeight;

    // host_colormap is loaded by Host_Init before it calls this.
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*(static_cast<const int *>(static_cast<const void *>(vid.colormap)) + 2048));

    vid.recalc_refdef = 1;
    modestate = MS_FULLSCREEN;

    // The viewport GL_BeginRendering reports. gl_screen.c owns these, and SCR_UpdateScreen
    // and the status bar read them.
    glx      = 0;
    gly      = 0;
    glwidth  = kScreenWidth;
    glheight = kScreenHeight;
}

void VID_Shutdown() {}

// One fixed video mode: nothing to keep in sync with, nothing to toggle or lock.
void VID_SyncCvars() {}
void VID_Toggle() {}
void VID_Lock() {}

// ------------------------------------------------------------------------------------------------
// Frame bracket
// ------------------------------------------------------------------------------------------------

void GL_BeginRendering(int * x, int * y, int * width, int * height)
{
    *x      = glx;
    *y      = gly;
    *width  = glwidth;
    *height = glheight;
}

void GL_EndRendering() {}

// QuakeSpasm's shader gamma pass, applied as the last step of every frame; nothing to do
// without shaders.
void GLSLGamma_GammaCorrect() {}

} // extern "C"

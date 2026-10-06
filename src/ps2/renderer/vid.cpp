/* ================================================================================================
 * File: vid.cpp
 * Brief: QuakeSpasm's VID_* video seam and the frame bracket SCR_UpdateScreen draws inside
 *        (GL_BeginRendering/GL_EndRendering). Brings the GS up, owns the engine's 'vid'
 *        screen state, and does the backend's per-frame work around the engine's drawing:
 *        the profiler's frame rollover, the test scene, the debug overlays, and the frame's
 *        submission.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/system/sys.h"
#include "ps2/system/heap.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/overlays.h"
#include "ps2/renderer/profile.h"
#include "ps2/tests/draw_cube.h"

#include <algorithm>
#include <utility>

#include <gs_psm.h>

// QuakeSpasm reads the cvars its video mode starts with out of config.cfg ahead of the configs;
// so does this, for the GS.
extern "C" {
#include "quake/cfgfile.h"
}

namespace {

// The PS2 framebuffer: 640x448 fits an NTSC field pair (and PAL's 512 lines with room to spare).
constexpr int kScreenWidth  = 640;
constexpr int kScreenHeight = 448;

// The frame's DMA chain, both halves in one block for the life of the program (see
// cmd_buffer.h). 64-byte aligned, a cache line, which the chain halves must start on.
constexpr u32 kChainBytes = 2u * ps2::cmdbuf::kHalfBytes;

// How the frame is steered, read every frame so both can be flipped live and judged on hardware.
//
// ps2_gs_latency leaves the frame drawing at EndFrame and shows it at the next one, at the cost of
// one frame of input lag. ps2_fb_dither hides the banding a 16-bit framebuffer shows on gradients.
cvar_t s_gsLatency    = ps2::MakeCvar("ps2_gs_latency", "1", CVAR_ARCHIVE);
cvar_t s_enableDither = ps2::MakeCvar("ps2_fb_dither",  "0", CVAR_ARCHIVE);

// The framebuffer format, which fixes the whole VRAM layout: read once, as the GS comes up, so a
// change takes effect on the next run.
cvar_t s_fb16Bit = ps2::MakeCvar("ps2_fb_16bit", "1", CVAR_ARCHIVE);

// VID_Init runs inside Host_Init, before quake.rc executes config.cfg, so the cvars the GS is
// brought up with are read from the file ahead of time, as QuakeSpasm does for its video mode.
// A "+ps2_fb_16bit 0" on the command line overrides the config.
void ReadInitCvars()
{
    const char * vars[] = { s_fb16Bit.name };

    if (CFG_OpenConfig("config.cfg") == 0)
    {
        CFG_ReadCvars(vars, ps2::ArrayLength(vars));
        CFG_CloseConfig();
    }
    CFG_ReadCvarOverrides(vars, ps2::ArrayLength(vars));
}

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
    Cvar_RegisterVariable(&s_gsLatency);
    Cvar_RegisterVariable(&s_enableDither);
    Cvar_RegisterVariable(&s_fb16Bit);
    ps2::debug::FrameLogInit();
    ps2::overlay::Init();
#if PS2_QUAKE_DEBUG
    ps2::test::RegisterCvars();
#endif // PS2_QUAKE_DEBUG

    ReadInitCvars();

    // The palette first: the GS builds its CLUTs from it as it comes up.
    ps2::texmgr::LoadPalette();
    ps2::tex::Init();

    void * const chain = ps2::heap::AllocAligned(ps2::heap::MemAlign(64), kChainBytes,
                                                 ps2::heap::MemTag::Renderer);

    // No lit-CLUT brightening: Quake's walls and skins draw at the palette's own colours.
    const ps2::gs::Config gsConfig = {
        .palette          = d_8to24table,
        .intensity        = 1.0f,
        .width            = kScreenWidth,
        .height           = kScreenHeight,
        .framebuffer16Bit = (s_fb16Bit.value != 0.0f)
    };
    ps2::rs::Init(gsConfig, chain, kChainBytes);

    vid.width    = ps2::gs::Width();
    vid.height   = ps2::gs::Height();
    vid.aspect   = static_cast<float>(vid.width) / static_cast<float>(vid.height);
    vid.numpages = 2;

    // The console's virtual size. SCR_Conwidth_f recomputes it from scr_conwidth and
    // scr_conscale as soon as a config sets either.
    vid.conwidth  = vid.width;
    vid.conheight = vid.height;

    // host_colormap is loaded by Host_Init before it calls this.
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*(static_cast<const int *>(static_cast<const void *>(vid.colormap)) + 2048));

    vid.recalc_refdef = 1;
    modestate = MS_FULLSCREEN;

    // The viewport GL_BeginRendering reports. gl_screen.c owns these, and SCR_UpdateScreen
    // and the status bar read them.
    glx      = 0;
    gly      = 0;
    glwidth  = vid.width;
    glheight = vid.height;

    Con_SafePrintf("GS: %dx%d, %d-bit framebuffer.\n", vid.width, vid.height,
                   (s_fb16Bit.value != 0.0f) ? 16 : 32);
}

void VID_Shutdown() {}

// One fixed video mode: nothing to keep in sync with, nothing to toggle or lock.
void VID_SyncCvars() {}
void VID_Toggle() {}
void VID_Lock() {}

// ------------------------------------------------------------------------------------------------
// Frame bracket
// ------------------------------------------------------------------------------------------------

// Opens the frame SCR_UpdateScreen is about to draw. That happens once per Host_Frame, and also
// from Con_Printf, which redraws the screen for each line printed while the client is not in a
// game - which is why nothing here may print with Con_Printf: a frame would open inside this one.
void GL_BeginRendering(int * x, int * y, int * width, int * height)
{
    // Close the frame the profile probes have been charging into, before any of this frame's
    // work is measured and before rs::BeginFrame opens the vsync probe.
    ps2::debug::ProfileNewFrame();

    // Snapshot that finished frame for the CSV log. Must sit between the rollover above and
    // rs::BeginFrame below, which is where the counters it reads get cleared.
    ps2::debug::FrameLogCapture();

    // 2D and 3D now draw freely until GL_EndRendering: 2D primitives open the pending 2D batch
    // lazily, and it flushes at each 2D->3D boundary and in rs::EndFrame.
    ps2::rs::BeginFrame(/*dither=*/s_enableDither.value != 0.0f);

    *x      = glx;
    *y      = gly;
    *width  = glwidth;
    *height = glheight;
}

void GL_EndRendering()
{
#if PS2_QUAKE_DEBUG
    // VU1 bring-up scene (cvar "ps2_testcube 1"): a 3D draw, so it flushes the 2D drawn so far
    // and lands on top of it.
    ps2::test::DrawRotatingCube();
#endif // PS2_QUAKE_DEBUG

    // The backend's own debug overlays, on top of everything. Each is cvar gated and returns at
    // once when off.
    ps2::overlay::Draw();

    ps2::rs::EndFrame(/*deferPresent=*/s_gsLatency.value != 0.0f);
}

// QuakeSpasm's shader gamma pass, applied as the last step of every frame; nothing to do
// without shaders.
void GLSLGamma_GammaCorrect() {}

// ------------------------------------------------------------------------------------------------
// Screenshots: what SCR_ScreenShot_f reads the screen with, and writes it out with
// ------------------------------------------------------------------------------------------------

// engine_hooks.h:
void PS2_ReadPixels(byte * rgb)
{
    const ps2::gs::DrawContext ctx = ps2::rs::FinishFrameInFlight();

    const int  width  = ps2::gs::Width();
    const int  height = ps2::gs::Height();
    const bool fb16   = (ps2::gs::FramebufferPsm() == GS_PSM_16);

    // In strips through one staging block, rather than the frame in one go: a 32-bit frame is
    // more than the 1 MB one DMA transfer can carry.
    constexpr int kStripRows = 32;
    const size_t stripBytes = static_cast<size_t>(width * kStripRows * (fb16 ? 2 : 4));
    void * const strip = ps2::heap::AllocAligned(ps2::heap::MemAlign(64), stripBytes,
                                                 ps2::heap::MemTag::Renderer);

    byte * out = rgb;
    for (int y = 0; y < height; y += kStripRows)
    {
        const int rows = std::min(kStripRows, height - y);
        ps2::gs::DownloadFramebufferRows(ctx, y, rows, strip);

        const int count = width * rows;
        if (fb16)
        {
            // PSMCT16: five bits each of red, green and blue from the bottom up, the alpha bit on
            // top. Each channel widens to eight bits by repeating its top bits, so 31 becomes 255.
            const u16 * const src = static_cast<const u16 *>(strip);
            for (int i = 0; i < count; ++i)
            {
                const u32 r = src[i] & 0x1Fu;
                const u32 g = (src[i] >> 5) & 0x1Fu;
                const u32 b = (src[i] >> 10) & 0x1Fu;
                *out++ = static_cast<byte>((r << 3) | (r >> 2));
                *out++ = static_cast<byte>((g << 3) | (g >> 2));
                *out++ = static_cast<byte>((b << 3) | (b >> 2));
            }
        }
        else
        {
            // PSMCT32: red, green, blue and alpha bytes.
            const u8 * const src = static_cast<const u8 *>(strip);
            for (int i = 0; i < count; ++i)
            {
                *out++ = src[(i * 4) + 0];
                *out++ = src[(i * 4) + 1];
                *out++ = src[(i * 4) + 2];
            }
        }
    }

    ps2::heap::Free(strip, stripBytes, ps2::heap::MemTag::Renderer);
}

// image.c's TGA writer, which went with the rest of image.c: uncompressed, with the channels
// swapped into TGA's blue-green-red order in place. 'upsidedown' marks the rows as top-down,
// where GL's readback came bottom-up.
qboolean Image_WriteTGA(const char * name, byte * data, int width, int height, int bpp, qboolean upsidedown)
{
    char pathname[MAX_OSPATH];

    Sys_mkdir(com_gamedir); // a 'game' switched to a directory that does not exist yet
    q_snprintf(pathname, sizeof(pathname), "%s/%s", com_gamedir, name);

    const int handle = Sys_FileOpenWrite(pathname);
    if (handle == -1)
    {
        return false;
    }

    byte header[18] = {};
    header[2]  = 2; // uncompressed true colour
    header[12] = static_cast<byte>(width & 255);
    header[13] = static_cast<byte>(width >> 8);
    header[14] = static_cast<byte>(height & 255);
    header[15] = static_cast<byte>(height >> 8);
    header[16] = static_cast<byte>(bpp);
    if (upsidedown)
    {
        header[17] = 0x20; // origin at the top left
    }

    const int bytes = bpp / 8;
    const int size  = width * height * bytes;
    for (int i = 0; i < size; i += bytes)
    {
        std::swap(data[i], data[i + 2]);
    }

    Sys_FileWrite(handle, header, static_cast<int>(sizeof(header)));
    Sys_FileWrite(handle, data, size);
    Sys_FileClose(handle);
    return true;
}

} // extern "C"

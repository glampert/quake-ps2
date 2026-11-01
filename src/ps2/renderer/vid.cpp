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
#include "ps2/debug/log_file.h"
#include "ps2/system/sys.h"
#include "ps2/system/iop_boot.h"
#include "ps2/system/heap.h"
#include "ps2/system/video_mode.h"
#include "ps2/renderer/gamma.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/texmgr.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/renderer/overlays.h"
#include "ps2/renderer/profile.h"
#include "ps2/renderer/view.h"
#include "ps2/renderer/vram.h"
#include "ps2/menu/page_layout.h"
#include "ps2/tests/draw_cube.h"
#include "ps2/save/save_notice.h"
#include "ps2/save/save_system.h"
#include "ps2/tests/map_cycle.h"
#include "ps2/tests/perf_run.h"
#include "ps2/tests/test_quit.h"
#include "ps2/tests/aio_test.h"
#include "ps2/fs/async_io.h"
#include "ps2/fs/aio_pump.h"
#include "ps2/fs/readahead_stream.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <gs_psm.h>

// QuakeSpasm reads the cvars its video mode starts with out of config.cfg ahead of the configs;
// so does this, for the GS.
extern "C" {
    #include "quake/cfgfile.h"
}

namespace {

// The PS2 framebuffer is 640 wide, and as tall as the TV standard shows: 448 lines for NTSC and
// 512 for PAL (video::VisibleLines).
constexpr int kScreenWidth = 640;

// The frame's DMA chain, both halves in one block for the life of the program (see
// cmd_buffer.h). 64-byte aligned, a cache line, which the chain halves must start on.
constexpr u32 kChainBytes = 2u * ps2::cmdbuf::kHalfBytes;

// How the frame is steered, read every frame so both can be flipped live and judged on hardware.
//
// ps2_gs_latency leaves the frame drawing at EndFrame and shows it at the next one, at the cost of
// one frame of input lag. ps2_fb_dither hides the banding a 16-bit framebuffer shows on gradients.
static cvar_t s_gsLatency    = ps2::MakeCvar("ps2_gs_latency", "1", CVAR_ARCHIVE);
static cvar_t s_enableDither = ps2::MakeCvar("ps2_fb_dither",  "0", CVAR_ARCHIVE);

// The framebuffer format, which fixes the whole VRAM layout, and the TV standard, which fixes the
// framebuffer's height and the field rate: read once, as the GS comes up, so a change takes
// effect on the next run. "auto" is the console's own standard (system/video_mode.h).
static cvar_t s_fb16Bit   = ps2::MakeCvar("ps2_fb_16bit",   "1",    CVAR_ARCHIVE);
static cvar_t s_videoMode = ps2::MakeCvar("ps2_video_mode", "auto", CVAR_ARCHIVE);

// Where the picture sits on the TV: pixels to the right and rows down of the standard's usual
// place, for a set that cuts an edge off (system/video_mode.h). Live, from the Video Options
// page's Screen X and Screen Y rows, and read ahead of the configs too, so the GS comes up with it.
static cvar_t s_screenX = ps2::MakeCvar("ps2_screen_x", "0", CVAR_ARCHIVE);
static cvar_t s_screenY = ps2::MakeCvar("ps2_screen_y", "0", CVAR_ARCHIVE);

constexpr int kMaxScreenX = 32; // pixels either way
constexpr int kMaxScreenY = 16; // rows either way

void ScreenOffsetChanged(cvar_t *)
{
    ps2::video::SetScreenOffset(std::clamp(static_cast<int>(s_screenX.value), -kMaxScreenX, kMaxScreenX),
                                std::clamp(static_cast<int>(s_screenY.value), -kMaxScreenY, kMaxScreenY));
}

// VID_Init runs inside Host_Init, before quake.rc executes config.cfg, so the cvars the GS is
// brought up with are read from the file ahead of time, as QuakeSpasm does for its video mode.
// A "+ps2_fb_16bit 0" on the command line overrides the config.
void ReadInitCvars()
{
    const char * vars[] = { s_fb16Bit.name, s_videoMode.name, s_screenX.name, s_screenY.name };

    ps2::save::ReadConfigCvars(vars, ps2::ArrayLength(vars));
    CFG_ReadCvarOverrides(vars, ps2::ArrayLength(vars));
    ScreenOffsetChanged(nullptr);
}

// ------------------------------------------------------------------------------------------------
// The Video Options page
// ------------------------------------------------------------------------------------------------
//
// QuakeSpasm's video module draws Options' "Video Options" page itself, through the vid_menu*fn
// hooks menu.c calls (gl_vidsdl.c's has the PC's modes). This one has the PS2's, laid out as
// QuakeSpasm's Options page is: the TV standard and the framebuffer format, which apply on the next
// boot, then the picture's place on the TV, the texture filtering, the dithering, the dynamic
// lights, the models' shadows, the particles' shape, the GS latency, the FPS counter and the debug
// panels, which apply at once. Each row steps its cvar through its values with left, right or Enter.

struct VideoRow
{
    const char *         label;
    const char *         help;   // under the rows while the cursor is on this one; 40 columns at most
    cvar_t *             cvar;
    const char * const * values; // what the row sets the cvar to, in order
    const char * const * names;  // what it shows for each; null for an off/on checkbox
    int                  count;

    // A row over more than one cvar, or over values VideoRowIndex can't tell apart, reads and sets
    // them through these instead; null for the rest.
    int  (*getIndex)();
    void (*setIndex)(int index);

    // A row over a signed number: the cvar holds it, from -count/2 to count/2, the row shows it
    // and the steps stop at the ends rather than wrap. The screen position rows.
    bool number;
};

constexpr const char * kVideoModes[]      = { "auto", "ntsc", "pal" };
constexpr const char * kVideoModeNames[]  = { "auto", "NTSC", "PAL" };
constexpr const char * kFbFormats[]       = { "1", "0" };
constexpr const char * kFbFormatNames[]   = { "16-bit", "32-bit" };
constexpr const char * kOffOn[]           = { "0", "1" };
constexpr const char * kMipFilters[]      = { "nearest", "bilinear", "trilinear" };
constexpr const char * kDlightModeNames[] = { "lightmaps", "vertex", "glows" };
constexpr const char * kParticleNames[]   = { "round", "square" };

// The dynamic lights row spans two cvars: gl_flashblend's glows stand in for the walls' lighting
// whichever way ps2_dynamic_lights says they take it, so picking the glows sets the one and leaves
// the other as it was.
int DynamicLightsIndex()
{
    if (gl_flashblend.value != 0.0f)
    {
        return 2;
    }
    return (q_strcasecmp(ps2::view::DynamicLightsCvar().string, "vertex") == 0) ? 1 : 0;
}

void SetDynamicLightsIndex(const int index)
{
    Cvar_Set(gl_flashblend.name, (index == 2) ? "1" : "0");
    if (index != 2)
    {
        Cvar_Set(ps2::view::DynamicLightsCvar().name, (index == 1) ? "vertex" : "lightmaps");
    }
}

// The particles row: r_particles draws discs at 1 and squares at 2, which VideoRowIndex would read
// as the same "on", so they read here as particles.cpp picks its texture: 2 is square and anything
// else round. 0, which draws none, is left to the console.
int ParticlesIndex()
{
    return (static_cast<int>(r_particles.value) == 2) ? 1 : 0;
}

void SetParticlesIndex(const int index)
{
    Cvar_Set(r_particles.name, (index == 1) ? "2" : "1");
}

const VideoRow kVideoRows[] = {
    { "Video mode",     "auto is the console's own; on restart",   &s_videoMode,                         kVideoModes, kVideoModeNames,  3,                     nullptr,            nullptr,               false },
    { "Color depth",    "16-bit leaves more VRAM; on restart",     &s_fb16Bit,                           kFbFormats,  kFbFormatNames,   2,                     nullptr,            nullptr,               false },
    { "Screen X",       "the picture left or right on the TV",     &s_screenX,                           nullptr,     nullptr,          (2 * kMaxScreenX) + 1, nullptr,            nullptr,               true  },
    { "Screen Y",       "the picture up or down on the TV",        &s_screenY,                           nullptr,     nullptr,          (2 * kMaxScreenY) + 1, nullptr,            nullptr,               true  },
    { "Texture filter", "trilinear blends mip levels too",         &ps2::view::MipFilterCvar(),          kMipFilters, kMipFilters,      3,                     nullptr,            nullptr,               false },
    { "Dithering",      "hides 16-bit color banding",              &s_enableDither,                      kOffOn,      nullptr,          2,                     nullptr,            nullptr,               false },
    { "Dynamic lights", "vertex: on VU1, no lightmap rebuilds",    nullptr,                              nullptr,     kDlightModeNames, 3,                     DynamicLightsIndex, SetDynamicLightsIndex, false },
    { "Model shadows",  "a shadow on the ground under each model", &r_shadows,                           kOffOn,      nullptr,          2,                     nullptr,            nullptr,               false },
    { "Particles",      "square: as software Quake drew them",     nullptr,                              nullptr,     kParticleNames,   2,                     ParticlesIndex,     SetParticlesIndex,     false },
    { "GS latency",     "a frame ahead: faster, one frame of lag", &s_gsLatency,                         kOffOn,      nullptr,          2,                     nullptr,            nullptr,               false },
    { "Show FPS",       "frames per second, at the top right",     &ps2::overlay::FpsCounterCvar(),      kOffOn,      nullptr,          2,                     nullptr,            nullptr,               false },
    { "Debug overlays", "memory, VRAM and profiling panels",       &ps2::overlay::DebugOverlaysCvar(),   kOffOn,      nullptr,          2,                     nullptr,            nullptr,               false },
};

constexpr int kNumVideoRows = ps2::ArrayLength(kVideoRows);

static int s_videoCursor = 0;

// Which of its values the row's cvar holds now: by name for the rows whose values are names, and
// for the rest, whose values are "0" and "1", by whether the cvar reads 0. One the row doesn't list
// reads as the first. A number row's index is its number from the bottom of its range.
int VideoRowIndex(const VideoRow & row)
{
    if (row.getIndex != nullptr)
    {
        return row.getIndex();
    }
    if (row.number)
    {
        return std::clamp(static_cast<int>(row.cvar->value) + (row.count / 2), 0, row.count - 1);
    }

    const bool byName = (row.values[0][0] < '0' || row.values[0][0] > '9');
    for (int i = 0; i < row.count; ++i)
    {
        const bool match = byName ? (q_strcasecmp(row.cvar->string, row.values[i]) == 0)
                                  : ((row.cvar->value != 0.0f) == (row.values[i][0] != '0'));
        if (match)
        {
            return i;
        }
    }
    return 0;
}

// Whether what the restart-only rows ask for differs from what the GS was set up with.
bool VideoNeedsRestart()
{
    const bool standardChanged = (ps2::video::ResolveStandard(s_videoMode.string, nullptr) != ps2::video::Selected());
    const bool formatChanged   = ((s_fb16Bit.value != 0.0f) != (ps2::gs::FramebufferPsm() == GS_PSM_16));
    return standardChanged || formatChanged;
}

void VideoMenuOpen()
{
    IN_Deactivate(modestate == MS_WINDOWED);
    key_dest     = key_menu;
    m_state      = m_video;
    m_entersound = true;
}

// The picture's edge, a white line along each side of the screen, while a screen position row is
// under the cursor: what the rows move, against the set's own edge. In screen pixels, whatever the
// menu's canvas, as the overlays draw.
void DrawScreenEdge()
{
    constexpr int kLine = 2;
    ps2::rs::FillRect(0, 0, glwidth, kLine, 255, 255, 255, 255);
    ps2::rs::FillRect(0, glheight - kLine, glwidth, kLine, 255, 255, 255, 255);
    ps2::rs::FillRect(0, 0, kLine, glheight, 255, 255, 255, 255);
    ps2::rs::FillRect(glwidth - kLine, 0, kLine, glheight, 255, 255, 255, 255);
}

void VideoMenuDraw()
{
    using namespace ps2::menu;

    qpic_t * const plaque = Draw_CachePic("gfx/qplaque.lmp");
    M_DrawTransPic(kPlaqueX, kPlaqueY, plaque);
    qpic_t * const banner = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - banner->width) / 2, 4, banner);

    for (int i = 0; i < kNumVideoRows; ++i)
    {
        const VideoRow & row = kVideoRows[i];
        const int y = kRowsY + (kRowHeight * i);

        // Labels end at column 22, as Options' do.
        M_Print(16 + ((22 - static_cast<int>(std::strlen(row.label))) * 8), y, row.label);
        if (row.number)
        {
            char text[8];
            const int value = VideoRowIndex(row) - (row.count / 2);
            if (value == 0)
            {
                q_strlcpy(text, "0", sizeof(text));
            }
            else
            {
                q_snprintf(text, sizeof(text), "%+d", value);
            }
            M_Print(220, y, text);
        }
        else if (row.names != nullptr)
        {
            M_Print(220, y, row.names[VideoRowIndex(row)]);
        }
        else
        {
            M_DrawCheckbox(220, y, VideoRowIndex(row));
        }
    }

    M_DrawCharacter(200, kRowsY + (kRowHeight * s_videoCursor), 12 + (static_cast<int>(realtime * 4.0) & 1));

    if (kVideoRows[s_videoCursor].number)
    {
        DrawScreenEdge();
    }

    // The row's help, centred under the rows and clear of the plaque (page_layout.h).
    const char * const help = kVideoRows[s_videoCursor].help;
    const int helpY = HelpLineY(kNumVideoRows, plaque->height);
    M_Print((320 - (static_cast<int>(std::strlen(help)) * 8)) / 2, helpY, help);
    if (VideoNeedsRestart())
    {
        constexpr const char * kRestart = "restart the game to apply the changes";
        M_PrintWhite((320 - (static_cast<int>(std::strlen(kRestart)) * 8)) / 2, helpY + 16, kRestart);
    }
}

void StepVideoRow(const int step)
{
    const VideoRow & row = kVideoRows[s_videoCursor];
    const int index = VideoRowIndex(row);
    const int next  = row.number ? std::clamp(index + step, 0, row.count - 1) : ((index + step + row.count) % row.count);
    if (row.setIndex != nullptr)
    {
        row.setIndex(next);
    }
    else if (row.number)
    {
        Cvar_SetValue(row.cvar->name, static_cast<float>(next - (row.count / 2)));
    }
    else
    {
        Cvar_Set(row.cvar->name, row.values[next]);
    }
}

void VideoMenuKey(const int key)
{
    switch (key)
    {
    case K_ESCAPE:
    case K_BBUTTON:
        M_Menu_Options_f();
        break;

    case K_UPARROW:
        S_LocalSound("misc/menu1.wav");
        s_videoCursor = (s_videoCursor + kNumVideoRows - 1) % kNumVideoRows;
        break;

    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        s_videoCursor = (s_videoCursor + 1) % kNumVideoRows;
        break;

    case K_LEFTARROW:
        S_LocalSound("misc/menu3.wav");
        StepVideoRow(-1);
        break;

    case K_RIGHTARROW:
    case K_ENTER:
    case K_KP_ENTER:
    case K_ABUTTON:
        S_LocalSound("misc/menu3.wav");
        StepVideoRow(1);
        break;

    default:
        break;
    }
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
    // The backend's own console commands; see RegisterCommands for why here. ps2_logfile too,
    // which the log's backlog waits on (debug/log_file.h).
    ps2::sys::RegisterCommands();
    ps2::debug::LogFileRegisterCvar();
    ps2::aio::RegisterCvars();
    ps2::aio::RegisterPumpCvar();
    ps2::fs::RegisterReadAheadCvar();
    ps2::save::Init();
    ps2::test::RegisterPerfTestCvar();
#if PS2_QUAKE_DEBUG || PS2_QUAKE_PROFILE
    ps2::test::RegisterTestQuitCvar();
#endif // PS2_QUAKE_DEBUG || PS2_QUAKE_PROFILE
#if PS2_QUAKE_DEBUG
    ps2::test::InitMapCycle();
    ps2::test::InitAioTest();
#endif // PS2_QUAKE_DEBUG

    // Host_Init queues "vid_unlock" to run after the configs: QuakeSpasm locks its video mode
    // while they do. And the config.cfg it writes ends in "vid_restart", to apply the mode it saved.
    // With one fixed mode there is nothing to lock, unlock or restart.
    Cmd_AddCommand("vid_unlock", []() {});
    Cmd_AddCommand("vid_restart", []() {});

    // The texture heap block by block, with its totals and its peak this run: the same dump a
    // failed allocation prints, on demand.
    Cmd_AddCommand("ps2_vramstats", []() { ps2::vram::DumpAllBlocks(); });

    Cvar_RegisterVariable(&vid_gamma);
    Cvar_RegisterVariable(&vid_contrast);
    Cvar_RegisterVariable(&s_gsLatency);
    Cvar_RegisterVariable(&s_enableDither);
    Cvar_RegisterVariable(&s_fb16Bit);
    Cvar_RegisterVariable(&s_videoMode);
    Cvar_RegisterVariable(&s_screenX);
    Cvar_RegisterVariable(&s_screenY);
    Cvar_SetCallback(&s_screenX, ScreenOffsetChanged);
    Cvar_SetCallback(&s_screenY, ScreenOffsetChanged);

    // Options' "Video Options" row shows once the video module draws the page.
    vid_menucmdfn  = VideoMenuOpen;
    vid_menudrawfn = VideoMenuDraw;
    vid_menukeyfn  = VideoMenuKey;

    ps2::debug::FrameLogInit();
    ps2::overlay::Init();

#if PS2_QUAKE_DEBUG
    ps2::test::RegisterCvars();
#endif // PS2_QUAKE_DEBUG

    ReadInitCvars();

    // The palette first: the GS builds its CLUTs from it as it comes up, through the Brightness and
    // Contrast curves, at 1 until the configs set them.
    ps2::tex::LoadPalette();
    ps2::gamma::Init();
    ps2::tex::Init();

    void * const chain = ps2::heap::AllocAligned(ps2::heap::MemAlign(64), kChainBytes,
                                                 ps2::heap::MemTag::Renderer);

    // NTSC or PAL, latched here like the framebuffer format. The height follows it: a PAL picture
    // shows 512 lines, and 448 of them would leave a band of black under the view.
    bool videoModeKnown = true;
    const ps2::video::Standard standard = ps2::video::SelectStandard(s_videoMode.string, &videoModeKnown);
    if (!videoModeKnown)
    {
        Con_SafePrintf("ps2_video_mode: '%s' is not auto, ntsc or pal; using the console's own, %s.\n",
                       s_videoMode.string, ps2::video::Name(standard));
    }

    const ps2::gs::Config gsConfig = {
        .palette          = ps2::gamma::Palette(),
        .lightRamp        = ps2::gamma::LightRamp(),
        .width            = kScreenWidth,
        .height           = ps2::video::VisibleLines(standard),
        .framebuffer16Bit = (s_fb16Bit.value != 0.0f),
        .standard         = standard
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

    Con_SafePrintf("GS: %dx%d %s, %d-bit framebuffer.\n", vid.width, vid.height, ps2::video::Name(standard),
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

    // The CLUTs, if the Brightness or Contrast slider moved them: between frames, with the one
    // left drawing finished first.
    ps2::gamma::ApplyPendingChange();

    // 2D and 3D now draw freely until GL_EndRendering: 2D primitives open the pending 2D batch
    // lazily, and it flushes at each 2D->3D boundary and in rs::EndFrame.
    ps2::rs::BeginFrame(/*dither=*/s_enableDither.value != 0.0f);

    // The textures the engine let go of while the last frame was being recorded: that frame has
    // been drawn by now, and nothing recorded since can reach them.
    ps2::tex::ReleaseRetiredTextures();

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

    // The save notice, over everything the engine drew. A frame with it is shown before this
    // returns, whatever ps2_gs_latency says: the save device's access that follows draws nothing
    // until it is done, so no later frame would put this one on screen (save/save_notice.h).
    const bool saveNotice = ps2::save::SaveNoticeShown();
    if (saveNotice)
    {
        ps2::save::DrawSaveNotice(/*onLoadingScreen=*/false);
    }

    // The backend's own debug overlays, on top of everything. Each is cvar gated and returns at
    // once when off.
    ps2::overlay::Draw();

    // SCR_ModalMessage's question ("start a new game?") is shown at once too: it draws this one
    // frame and then waits for the answer, so a frame left for the next to show would never be.
    const bool dialog = (scr_drawdialog != 0);

    ps2::rs::EndFrame(/*deferPresent=*/s_gsLatency.value != 0.0f && !saveNotice && !dialog);
}

// QuakeSpasm's shader gamma pass, applied as the last step of every frame; nothing to do
// without shaders.
void GLSLGamma_GammaCorrect() {}

} // extern "C"

// ------------------------------------------------------------------------------------------------
// The loading screen's frames (loading_screen.h)
// ------------------------------------------------------------------------------------------------

// The plaque, as SCR_DrawLoading draws it, and the status bar along the bottom: a line of console
// text on a dark band, a few pixels up so a TV's overscan leaves it; and the save notice
// while it is up. The client draws nothing while the plaque is up, so no later frame would put
// this one on screen: it is shown before returning, whatever ps2_gs_latency says. Neither the view
// nor the status bar goes in, as the client's state is being torn down and rebuilt under them;
// behind the plaque is the console background's copper brown.
bool ps2::loading::DrawLoadingScreen(const char * status)
{
    if (ps2::rs::IsFrameOpen())
    {
        return false;
    }

    ps2::gamma::ApplyPendingChange();
    ps2::rs::BeginFrame(/*dither=*/s_enableDither.value != 0.0f);
    ps2::tex::ReleaseRetiredTextures();

    // The most common colour in gfx/conback.lmp, (47, 23, 11), rather than r_clearcolor's grey,
    // which is the 3D view's.
    constexpr int kBackgroundColour = 97;

    GL_Set2D();
    GL_SetCanvas(CANVAS_DEFAULT);
    Draw_Fill(0, 0, glwidth, glheight, kBackgroundColour, 1.0f);

    GL_SetCanvas(CANVAS_MENU);
    qpic_t * const pic = Draw_CachePic("gfx/loading.lmp");
    Draw_Pic((320 - pic->width) / 2, (240 - 48 - pic->height) / 2, pic);

    constexpr int kGlyphSize = 8;
    constexpr int kBarPadY   = 4;
    constexpr int kBarColour = 4; // a dark grey in Quake's palette

    GL_SetCanvas(CANVAS_DEFAULT);
    const int barY = glheight - (kGlyphSize + kBarPadY);
    Draw_Fill(0, barY, glwidth, kGlyphSize, kBarColour, 1.0f);

    const int maxChars = glwidth / kGlyphSize;
    const int numChars = std::min(static_cast<int>(std::strlen(status)), maxChars);
    const int textX    = ((maxChars - numChars) / 2) * kGlyphSize;
    for (int i = 0; i < numChars; ++i)
    {
        Draw_Character(textX + (i * kGlyphSize), barY, static_cast<unsigned char>(status[i]));
    }

    // The save notice, under the plaque, while the load menu's load reads the save device.
    if (ps2::save::SaveNoticeShown())
    {
        ps2::save::DrawSaveNotice(/*onLoadingScreen=*/true);
    }

    ps2::rs::EndFrame(/*deferPresent=*/false);
    return true;
}

extern "C" {

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

    // On the HDD a closed file's metadata can still be cached by PFS (see SyncGameDataDevice).
    ps2::sys::SyncGameDataDevice();
    return true;
}

} // extern "C"

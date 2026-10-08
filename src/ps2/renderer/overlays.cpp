/* ================================================================================================
 * File: overlays.cpp
 * Brief: The backend's on-screen debug overlays. See overlays.h.
 *
 *        Drawn straight onto the framebuffer in pixels with the console font, outside the engine's
 *        canvases, and charged to their own "Overlay" event rather than the engine's "Ui", so a
 *        capture can tell the instrumentation's cost from the thing being measured.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/overlays.h"
#include "ps2/renderer/draw.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/vram.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/heap.h"

#include <algorithm>
#include <cstdio>

namespace {

// One console font glyph, and the line spacing the panels use.
constexpr int kGlyphSize  = 8;
constexpr int kLineHeight = kGlyphSize + 2;

// Vertex colour for the text (GS modulate: 128 = texels unchanged).
constexpr u8 kWhite[3]  = { 128, 128, 128 };
constexpr u8 kYellow[3] = { 128, 128, 0   };
constexpr u8 kGreen[3]  = { 0,   128, 0   };
constexpr u8 kRed[3]    = { 128, 0,   0   };

cvar_t s_showDebugOverlays = ps2::MakeCvar("ps2_debug_overlays", PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE); // all but the FPS counter
cvar_t s_showFpsCount      = ps2::MakeCvar("ps2_show_fps",       PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE);
cvar_t s_showMemStats      = ps2::MakeCvar("ps2_show_memstats",  PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE);
cvar_t s_showVramStats     = ps2::MakeCvar("ps2_show_vramstats", PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE);
cvar_t s_showDrawStats     = ps2::MakeCvar("ps2_show_drawstats", PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE);
cvar_t s_showProfileStats  = ps2::MakeCvar("ps2_show_profile",   PS2_QUAKE_DEBUG ? "1" : "0", CVAR_ARCHIVE);

// One 8x8 glyph, at a screen position.
void DrawGlyph(const int x, const int y, int c, const u8 color[3])
{
    c &= 255;
    if ((c & 127) == ' ' || y <= -kGlyphSize)
    {
        return; // whitespace, or off the top
    }

    const int row = (c >> 4) * kGlyphSize;
    const int col = (c & 15) * kGlyphSize;
    ps2::rs::DrawTexturedRect(ps2::draw::Conchars(), x, y, kGlyphSize, kGlyphSize,
                              col, row, col + kGlyphSize, row + kGlyphSize, color);
}

void DrawText(int x, int y, const char * str, const u8 color[3] = kWhite)
{
    const int initialX = x;
    for (; *str != '\0'; ++str)
    {
        DrawGlyph(x, y, *str, color);
        x += kGlyphSize;
        if (*str == '\n')
        {
            y += kLineHeight;
            x = initialX;
        }
    }
}

// A black panel to give the text more contrast.
void DrawPanel(const int x, const int y, const int width, const int height)
{
    ps2::rs::FillRect(x, y, width, height, 0, 0, 0, 255);
}

// Frames-per-second counter at the top-right corner of the screen.
//
// Read off gs::PresentClock rather than a clock sampled here. Its stamps sit on the vsyncs the
// frames went up on, so the rate comes out exact; a sample taken here lands after however much of
// the frame came first, which moved enough between frames that a 4-frame average read 61 on a
// third of its updates and 59 on one in twenty with every frame on time at 59.94 Hz. Each reading
// averages half a second, and is replaced as often.
//
// The colour counts missed vsyncs instead of comparing against 60, which NTSC's 59.94 Hz sits just
// under and PAL's 50 Hz never reaches: green while every frame went up on the field after the
// last, yellow once one did not, red at half the refresh rate or below.
void DrawFpsCounter()
{
    // Restarted each time the counter is switched on, so the time it spent hidden never lands in
    // an average.
    static struct
    {
        bool                  active;
        bool                  published;
        ps2::gs::PresentClock windowStart;
        int                   fps;
        const u8 *            color;
    } s_fps;

    if (s_showFpsCount.value == 0.0f)
    {
        s_fps.active = false;
        return;
    }

    const ps2::gs::PresentClock & clock = ps2::gs::GetPresentClock();

    // A window opens on a present, so both its ends are vsyncs. None has happened yet when the
    // counter comes on at boot, so it waits for the first.
    if (!s_fps.active || s_fps.windowStart.frames == 0)
    {
        s_fps.active      = true;
        s_fps.published   = false;
        s_fps.windowStart = clock;
    }

    const u32 elapsed = clock.lastTicks - s_fps.windowStart.lastTicks;
    if (elapsed >= (ps2::gs::kPresentTicksPerSec / 2u))
    {
        const u32 frames = clock.frames - s_fps.windowStart.frames;
        const u32 fields = clock.fields - s_fps.windowStart.fields;

        // 32-bit: the R5900 has no 64-bit multiply or divide. A window holds a few dozen frames at
        // most, nowhere near overflowing frames * 576000.
        s_fps.fps         = static_cast<int>(((frames * ps2::gs::kPresentTicksPerSec) + (elapsed / 2u)) / elapsed);
        s_fps.color       = (fields == frames) ? kGreen : (fields >= (2u * frames)) ? kRed : kYellow;
        s_fps.published   = true;
        s_fps.windowStart = clock;
    }

    char text[32] = "FPS --"; // until the first window closes
    const u8 * color = kWhite;
    if (s_fps.published)
    {
        std::snprintf(text, sizeof(text), "FPS %d", s_fps.fps);
        color = s_fps.color;
    }

    DrawPanel(ps2::gs::Width() - 68, 2, 64, 12);
    DrawText(ps2::gs::Width() - 64, 4, text, color);
}

// Per-event frame time panel, stacked under the FPS counter in the top-right corner: one line per
// PS2_PROFILE_SCOPED_EVENT site flagged kScreenOverlay, showing what that site cost over one frame.
//
// The numbers are the previous frame's, not a running total or an average: GL_BeginRendering
// rolls the accumulators over, so the frame being reported is complete by the time this draws -
// down to the probes that only close after it (VSync, Frame). A site that never ran in that frame
// reads 0.000, and one that ran several times shows the sum of its calls.
void DrawProfileOverlay()
{
#if PS2_QUAKE_PROFILE
    if (s_showProfileStats.value == 0.0f)
    {
        return;
    }

    // Capped so a heavily instrumented build can't run off the screen: at 32 rows the panel is
    // 338 pixels tall, which still clears the bottom of a 448-line frame.
    constexpr int kMaxRows = 32;

    const ps2::debug::ProfileEvent * rows[kMaxRows];
    int numRows = 0;

    for (const auto * ev = ps2::debug::ProfileEventList(); ev != nullptr && numRows < kMaxRows; ev = ev->next)
    {
        if (ev->flags & ps2::debug::kScreenOverlay)
        {
            rows[numRows++] = ev;
        }
    }

    std::sort(rows, rows + numRows,
              [](const ps2::debug::ProfileEvent * a, const ps2::debug::ProfileEvent * b) -> bool
              {
                  return a->sortKey < b->sortKey;
              });

    if (numRows == 0)
    {
        return; // nothing instrumented has been reached yet
    }

    constexpr int kPanelWidth = 148;
    constexpr int kPadding    = 4;

    const int panelHeight = ((numRows + 1) * kLineHeight) + (kPadding * 2); // header + one per event
    const int panelX      = ps2::gs::Width() - kPanelWidth; // right edge aligned with the FPS counter
    const int panelY      = 16;                             // clears the 12px FPS box at y = 2

    DrawPanel(panelX, panelY, kPanelWidth, panelHeight);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawText(textX, textY, "FRAME TIMES (ms)");
    textY += kLineHeight;

    char line[64];
    char millisec[16];
    for (int i = 0; i < numRows; ++i)
    {
        const auto * const ev = rows[i];
        std::snprintf(line, sizeof(line), "%-10s %6s", ev->name,
                      ps2::debug::ProfileFormatMillisec(ev->lastFrameCycles, millisec, sizeof(millisec)));

        const u8 * color = kWhite;
        if (ev == &ps2::prof_evt::Frame)
        {
            const auto ms = ev->FrameMilliseconds();
            color = (ms > 33) ? kRed : (ms > 16) ? kYellow : kGreen; // below 30 / 60 fps
        }

        DrawText(textX, textY, line, color);
        textY += kLineHeight;
    }
#endif // PS2_QUAKE_PROFILE
}

// Memory panel in the lower-right corner: one line per heap tag with its running byte total, then
// the total across all tags, its high-water and what the heap has left.
void DrawMemUsageOverlay()
{
    if (s_showMemStats.value == 0.0f)
    {
        return;
    }

    constexpr int kNumLines   = static_cast<int>(ps2::heap::MemTag::TagCount) + 4; // header + tags + total, peak, left
    constexpr int kPanelWidth = 176;
    constexpr int kPadding    = 4;

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);
    const int panelX      = ps2::gs::Width() - kPanelWidth;
    const int panelY      = ps2::gs::Height() - panelHeight;

    DrawPanel(panelX, panelY, kPanelWidth, panelHeight);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawText(textX, textY, "MEM USAGE");
    textY += kLineHeight;

    char line[64];
    char unit[ps2::heap::kMemUnitStrSize];
    size_t totalBytes = 0;
    for (int i = 0; i < static_cast<int>(ps2::heap::MemTag::TagCount); ++i)
    {
        const auto tag = static_cast<ps2::heap::MemTag>(i);
        const size_t tagBytes = ps2::heap::GetStatsForMemTag(tag).totalBytes;
        totalBytes += tagBytes;

        std::snprintf(line, sizeof(line), "%-10s %s", ps2::heap::GetNameForMemTag(tag),
                      ps2::heap::FormatMemoryUnit(tagBytes, true, unit, sizeof(unit)));
        DrawText(textX, textY, line);
        textY += kLineHeight;
    }

    std::snprintf(line, sizeof(line), "%-10s %s", "Total",
                  ps2::heap::FormatMemoryUnit(totalBytes, true, unit, sizeof(unit)));
    DrawText(textX, textY, line);
    textY += kLineHeight;

    // The high-water of Total, which is what decides whether a map change fits: the moment the old
    // map is still resident while the new one loads is long gone by the time anyone reads Total.
    std::snprintf(line, sizeof(line), "%-10s %s", "Peak",
                  ps2::heap::FormatMemoryUnit(ps2::heap::GetPeakMemBytes(), true, unit, sizeof(unit)));
    DrawText(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %s", "Sbrk Left",
                  ps2::heap::FormatMemoryUnit(ps2::heap::GetAvailableMemBytes(), true, unit, sizeof(unit)));
    DrawText(textX, textY, line);
}

// GS VRAM panel in the lower-left corner: how much of the texture heap is committed, the resident
// textures, and this frame's uploads (streaming pressure) and the GS drains a full heap forced
// (see rs::EnsureTextureResident; anything but zero means the frame's working set does not fit).
void DrawVramUsageOverlay()
{
    if (s_showVramStats.value == 0.0f)
    {
        return;
    }

    constexpr int kNumLines   = 5; // header + four stats
    constexpr int kPanelWidth = 174;
    constexpr int kPadding    = 4;

    const ps2::vram::Stats stats = ps2::vram::GetStats();

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);
    const int panelX      = 0;
    const int panelY      = ps2::gs::Height() - panelHeight;

    DrawPanel(panelX, panelY, kPanelWidth, panelHeight);

    const int textX = panelX + kPadding;
    int textY = panelY + kPadding;

    DrawText(textX, textY, "VRAM USAGE");
    textY += kLineHeight;

    char line[64];
    char unit[ps2::heap::kMemUnitStrSize];

    std::snprintf(line, sizeof(line), "%-10s %s", "Used",
                  ps2::heap::FormatMemoryUnit(static_cast<size_t>(stats.totalWords - stats.freeWords) * 4u,
                                              true, unit, sizeof(unit)));
    DrawText(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %s", "Total",
                  ps2::heap::FormatMemoryUnit(static_cast<size_t>(stats.totalWords) * 4u, true, unit, sizeof(unit)));
    DrawText(textX, textY, line);
    textY += kLineHeight;

    std::snprintf(line, sizeof(line), "%-10s %d", "Resident", stats.residentTextures);
    DrawText(textX, textY, line);
    textY += kLineHeight;

    // Uploads and heap-full drains share a line; the drain count is the one to watch, since it is
    // zero on a healthy frame.
    std::snprintf(line, sizeof(line), "%-10s %d (%d sync)", "Uploads", stats.uploadsThisFrame,
                  stats.oomSyncsThisFrame);
    DrawText(textX, textY, line);
}

// Draw statistics panel in the top-left corner: what the last frame submitted to the GS and VU1.
void DrawDrawStatsOverlay()
{
#if PS2_QUAKE_PROFILE
    if (s_showDrawStats.value == 0.0f)
    {
        return;
    }

    const ps2::rs::DrawStats & rcStats = ps2::rs::GetStats();

    const struct
    {
        const char * label;
        int value;
    } rows[] = {
        { "Tris",     rcStats.trisDrawn    },
        { "Prts",     rcStats.particles    },
        { "Batches",  rcStats.drawBatches  },
        // Most qwords one GIF block in the frame chain has ever held - the 2D in practice, since
        // the clear is a fixed ~30. A slice of the chain rather than a budget of its own, so what
        // it says is how much of a chain half a full console wants on top of the 3D.
        { "Gif2DPk", ps2::rs::Gif2DPeakQwords() },
        // The frame DMA chain: high-water in KB against its capacity, how many times it was
        // kicked last frame, and how many of those kicks were the overflow emergency rather than
        // the end of the frame. One kick and no emergency drains is the target; a drain every
        // frame means cmdbuf::kHalfBytes is too small for the scene.
        { "ChainKB",  static_cast<int>(ps2::cmdbuf::PeakBytes() / 1024u) },
        { "ChainKck", ps2::cmdbuf::KicksLastFrame()                      },
        { "ChainDrn", ps2::cmdbuf::EmergencyDrainsLastFrame()            },
    };

    constexpr int kPanelWidth = 136;
    constexpr int kPadding    = 4;
    constexpr int kNumLines   = ps2::ArrayLength(rows) + 1; // header + one per counter

    const int panelHeight = (kNumLines * kLineHeight) + (kPadding * 2);
    DrawPanel(0, 0, kPanelWidth, panelHeight);

    const int textX = kPadding;
    int textY = kPadding;

    DrawText(textX, textY, "DRAW STATS");
    textY += kLineHeight;

    char line[64];
    for (const auto & row : rows)
    {
        std::snprintf(line, sizeof(line), "%-8s %6d", row.label, row.value);
        DrawText(textX, textY, line);
        textY += kLineHeight;
    }
#endif // PS2_QUAKE_PROFILE
}

} // namespace

namespace ps2::overlay {

void Init()
{
    Cvar_RegisterVariable(&s_showDebugOverlays);
    Cvar_RegisterVariable(&s_showFpsCount);
    Cvar_RegisterVariable(&s_showMemStats);
    Cvar_RegisterVariable(&s_showVramStats);
    Cvar_RegisterVariable(&s_showDrawStats);
    Cvar_RegisterVariable(&s_showProfileStats);
}

void Draw()
{
    PS2_PROFILE_SCOPED_EVENT(prof_evt::Overlay);

    // The FPS counter can be toggled separately from the rest.
    DrawFpsCounter();

    if (s_showDebugOverlays.value != 0.0f)
    {
        DrawProfileOverlay();
        DrawMemUsageOverlay();
        DrawVramUsageOverlay();
        DrawDrawStatsOverlay();
    }
}

} // namespace ps2::overlay

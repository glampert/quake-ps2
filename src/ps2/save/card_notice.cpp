/* ================================================================================================
 * File: card_notice.cpp
 * Brief: The memory card notice. See card_notice.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/card_notice.h"
#include "ps2/save/memcard.h"
#include "ps2/audio/cd_audio.h"
#include "ps2/engine_hooks.h"
#include "ps2/input/rumble.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/system/sys.h"

#include <algorithm>

namespace ps2::save {
namespace {

// However quick the card was, the notice stays up this long, so that it can be read.
constexpr int kMinShownMsec = 1000;

// What is happening, then what not to do meanwhile, which is printed white, as the quit box's
// last line is. The box is as wide as the longest line.
constexpr int kNumLines       = 5;
constexpr int kFirstWhiteLine = 3;

constexpr const char * kSavingLines[kNumLines] = {
    "Data is being saved to the",
    "Memory Card in slot 1.",
    "",
    "Do not remove the Memory Card",
    "or reset/switch off the console.",
};

constexpr const char * kLoadingLines[kNumLines] = {
    "Data is being loaded from the",
    "Memory Card in slot 1.",
    "",
    "Do not remove the Memory Card",
    "or reset/switch off the console.",
};

constexpr int TextLength(const char * text)
{
    int length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

constexpr int LongestLine(const char * const (&lines)[kNumLines])
{
    int longest = 0;
    for (const char * line : lines)
    {
        longest = std::max(longest, TextLength(line));
    }
    return longest;
}

// The layout, on the menu canvas: 320x200, at the menus' scale. The box's inside is in 8-pixel
// characters, the longest line with a space either side, and even, since M_DrawTextBox draws its
// middle two characters at a time; the border takes a character on every side.
constexpr int kGlyph     = 8;
constexpr int kLongest   = std::max(LongestLine(kSavingLines), LongestLine(kLoadingLines));
constexpr int kBoxChars  = (kLongest + 3) & ~1;
constexpr int kBoxWidth  = (kBoxChars + 2) * kGlyph;
constexpr int kBoxHeight = (kNumLines + 2) * kGlyph;
constexpr int kBoxLeft   = (320 - kBoxWidth) / 2;

static_assert(kBoxLeft >= 0, "The notice's lines are too long for the menu canvas.");

// Over the game the box is centred. On the loading screen it goes under the plaque, which
// SCR_DrawLoading centres higher up: gfx/loading.lmp is 24 lines tall, at line 84.
constexpr int kBoxTopOverGame        = (200 - kBoxHeight) / 2;
constexpr int kBoxTopOnLoadingScreen = 120;

static bool       s_shown     = false;
static CardAccess s_access    = CardAccess::Saving;
static int        s_shownMsec = 0; // ps2::sys::Milliseconds when it went up.

// A frame with the notice in it, on screen before this returns: the loading screen's while the
// plaque is up, the game's otherwise, which GL_EndRendering draws the notice over and shows at
// once.
void ShowFrame()
{
    if (!ps2::loading::Redraw())
    {
        SCR_UpdateScreen();
    }
}

} // namespace

bool BeginCardNotice(const CardAccess access)
{
    if (s_shown)
    {
        return true;
    }

    // The probe and the frame below hold the main thread up for some tens of milliseconds each, a
    // round trip to the card and the frame's vsyncs, while the sound feeder plays what the mixer
    // painted ahead (_snd_mixahead, 0.1 s). Topped up before each, it lasts until the music's fade
    // begins; without that, it ran out first in PCSX2, and the music stopped dead.
    S_ExtraUpdate();

    // No card, or one that can't be used, fails the access at once: no notice for that.
    if (!GetMemoryCardDevice().Probe())
    {
        return false;
    }

    s_shown  = true;
    s_access = access;

    // The game stands still from here until EndCardNotice, with no frame to stop a motor.
    ps2::input::StopRumble();

    S_ExtraUpdate();
    ShowFrame();
    s_shownMsec = ps2::sys::Milliseconds();

    ps2::audio::HoldMusic();
    return true;
}

void EndCardNotice()
{
    if (!s_shown)
    {
        return;
    }

    const int shownMsec = ps2::sys::Milliseconds() - s_shownMsec;
    if (shownMsec < kMinShownMsec)
    {
        Sys_Sleep(static_cast<unsigned long>(kMinShownMsec - shownMsec));
    }
    Con_DPrintf("Card notice: down after %d ms.\n", ps2::sys::Milliseconds() - s_shownMsec);

    s_shown = false;

    // Off the loading screen at once. Over the game, the next frame is drawn without it.
    ps2::loading::Redraw();
    ps2::audio::ReleaseMusic();
}

bool CardNoticeShown()
{
    return s_shown;
}

void DrawCardNotice(const bool onLoadingScreen)
{
    if (!onLoadingScreen)
    {
        Draw_FadeScreen();
    }

    GL_SetCanvas(CANVAS_MENU);
    const int top = onLoadingScreen ? kBoxTopOnLoadingScreen : kBoxTopOverGame;
    M_DrawTextBox(kBoxLeft, top, kBoxChars, kNumLines);

    const char * const * const lines = (s_access == CardAccess::Saving) ? kSavingLines : kLoadingLines;
    for (int i = 0; i < kNumLines; ++i)
    {
        const int x = (320 - (TextLength(lines[i]) * kGlyph)) / 2;
        const int y = top + ((i + 1) * kGlyph);
        if (i < kFirstWhiteLine)
        {
            M_Print(x, y, lines[i]);
        }
        else
        {
            M_PrintWhite(x, y, lines[i]);
        }
    }
}

} // namespace ps2::save

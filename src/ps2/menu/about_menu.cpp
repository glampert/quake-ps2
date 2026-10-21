/* ================================================================================================
 * File: about_menu.cpp
 * Brief: The main menu's "About" page: what this port is, its version, who made it and where it
 *        lives, in a message box under the item's own lettering. Its item comes after
 *        Help/Ordering, the main menu having lost Multiplayer, which the PS2 doesn't play; the help
 *        pages are pictures in the game data, and this one is text. menu.c opens it from the main
 *        menu and hands it its frames and keys (engine_hooks.h), as it does the debug page's.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

// A line of the box: the menus' gold text, or white for what stands out.
struct Line
{
    const char * text;
    bool         white;
};

// QUAKESPASM_VERSION is the port's own version (quakedef.h); 0.97.0 is the QuakeSpasm it started
// from.
constexpr Line kLines[] = {
    { "Quake for the PlayStation 2",    true  },
    { "Version " QUAKESPASM_VER_STRING, false },
    { "",                               false },
    { "An unofficial, fan-made port",   false },
    { "of id Software's Quake, built",  false },
    { "on QuakeSpasm 0.97.0 with only", false },
    { "the free PS2DEV SDK.",           false },
    { "",                               false },
    { "PS2 port by Guilherme Lampert",  false },
    { "",                               false },
    { "github.com/glampert/quake-ps2",  true  },
};

constexpr int kNumLines = ps2::ArrayLength(kLines);

// On the menu canvas (320x200): the title where the main menu's sits, and under it the box,
// centred, as wide as the longest line with a space either side (and even: M_DrawTextBox draws
// its middle two columns at a time). The help pages have no QUAKE plaque down the left, and
// neither has this one, which leaves the box room for the web address.
constexpr int kGlyphSize = 8;
constexpr int kTitleY    = 6;
constexpr int kBoxY      = 32;

constexpr int LongestLine()
{
    int longest = 0;
    for (const Line & line : kLines)
    {
        longest = std::max(longest, static_cast<int>(std::char_traits<char>::length(line.text)));
    }
    return longest;
}

constexpr int kBoxColumns = (LongestLine() + 3) & ~1;
constexpr int kBoxX       = (320 - ((kBoxColumns + 2) * kGlyphSize)) / 2;
static_assert(kBoxX >= 0, "The About page's lines are too long for the menu canvas.");

} // namespace

// ------------------------------------------------------------------------------------------------
// engine_hooks.h
// ------------------------------------------------------------------------------------------------

void PS2_AboutMenuOpen()
{
    key_dest     = key_menu;
    m_state      = m_ps2about;
    m_entersound = true;
}

void PS2_AboutMenuDraw()
{
    qpic_t * const title = Draw_CachePic("gfx/menuabout.lmp");
    M_DrawTransPic((320 - title->width) / 2, kTitleY, title);

    M_DrawTextBox(kBoxX, kBoxY, kBoxColumns, kNumLines);
    for (int i = 0; i < kNumLines; ++i)
    {
        const Line & line = kLines[i];
        const int x = (320 - (static_cast<int>(std::strlen(line.text)) * kGlyphSize)) / 2;
        const int y = kBoxY + (kGlyphSize * (i + 1));
        if (line.white)
        {
            M_PrintWhite(x, y, line.text);
        }
        else
        {
            M_Print(x, y, line.text);
        }
    }
}

// Back, Start or Cross go back to the main menu: the page has nothing to pick.
void PS2_AboutMenuKey(const int key)
{
    switch (key)
    {
    case K_ESCAPE:
    case K_BBUTTON:
    case K_ENTER:
    case K_KP_ENTER:
    case K_ABUTTON:
        M_Menu_Main_f();
        break;

    default:
        break;
    }
}

/* ================================================================================================
 * File: save_notice.cpp
 * Brief: The save notice. See save_notice.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_notice.h"
#include "ps2/save/save_system.h"
#include "ps2/audio/cd_audio.h"
#include "ps2/builtin/builtin.h"
#include "ps2/engine_hooks.h"
#include "ps2/input/rumble.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/splash_screen.h"
#include "ps2/system/sys.h"

#include <algorithm>
#include <cstdlib>

namespace ps2::save {
namespace {

// However quick the device was, the notice stays up this long, so that it can be read.
constexpr int kMinShownMsec = 1000;

// What is happening, then what not to do meanwhile, which is printed white, as the quit box's
// last line is: the first line, the device, a gap and the warning. The box is as wide as the
// longest line any device has, so it is the same box whatever it names.
constexpr int kNumLines       = 5;
constexpr int kFirstWhiteLine = 3;

constexpr const char * kSavingLine  = "Data is being saved to the";
constexpr const char * kLoadingLine = "Data is being loaded from the";

// By DeviceKind.
constexpr int kNumKinds = 4;
static_assert(static_cast<int>(DeviceKind::HardDisk) + 1 == kNumKinds, "A line for every DeviceKind.");

constexpr const char * kDeviceLines[kNumKinds] = {
    "Memory Card in slot 1.",
    "host filesystem.",
    "USB drive.",
    "hard disk drive.",
};

constexpr const char * kWarningLines[kNumKinds][2] = {
    { "Do not remove the Memory Card", "or reset/switch off the console." },
    { "Do not reset or switch off",    "the console."                     },
    { "Do not remove the USB drive",   "or reset/switch off the console." },
    { "Do not reset or switch off",    "the console."                     },
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

constexpr int LongestLine()
{
    int longest = std::max(TextLength(kSavingLine), TextLength(kLoadingLine));
    for (int kind = 0; kind < kNumKinds; ++kind)
    {
        longest = std::max(longest, TextLength(kDeviceLines[kind]));
        longest = std::max(longest, TextLength(kWarningLines[kind][0]));
        longest = std::max(longest, TextLength(kWarningLines[kind][1]));
    }
    return longest;
}

// The layout, on the menu canvas: 320x200, at the menus' scale. The box's inside is in 8-pixel
// characters, the longest line with a space either side, and even, since M_DrawTextBox draws its
// middle two characters at a time; the border takes a character on every side.
constexpr int kGlyph     = 8;
constexpr int kLongest   = LongestLine();
constexpr int kBoxChars  = (kLongest + 3) & ~1;
constexpr int kBoxWidth  = (kBoxChars + 2) * kGlyph;
constexpr int kBoxHeight = (kNumLines + 2) * kGlyph;
constexpr int kBoxLeft   = (320 - kBoxWidth) / 2;

static_assert(kBoxLeft >= 0, "The notice's lines are too long for the menu canvas.");

// Over the game the box is centred. On the loading screen it goes under the plaque, which
// SCR_DrawLoading centres higher up: gfx/loading.lmp is 24 lines tall, at line 84.
constexpr int kBoxTopOverGame        = (200 - kBoxHeight) / 2;
constexpr int kBoxTopOnLoadingScreen = 120;

// Where the notice went up: what EndSaveNotice has to take it off.
enum class Where : u8
{
    Nowhere,       // Nothing could be drawn (inside a frame, between gs::Init and SCR_Init): it
                   // doesn't go up, and the device is left alone.
    Game,          // Over the game's frame.
    LoadingScreen, // On the loading screen.
    Splash,        // Over the boot splash.
};

static bool       s_shown     = false;
static SaveAccess s_access    = SaveAccess::Saving;
static DeviceKind s_kind      = DeviceKind::MemoryCard;
static Where      s_where     = Where::Nowhere;
static int        s_shownMsec = 0; // ps2::sys::Milliseconds when it went up.

// The lines for the access and the device the notice is up for.
struct Lines
{
    const char * line[kNumLines];
};

Lines NoticeLines()
{
    const int kind = static_cast<int>(s_kind);
    return { { (s_access == SaveAccess::Saving) ? kSavingLine : kLoadingLine, kDeviceLines[kind], "",
               kWarningLines[kind][0], kWarningLines[kind][1] } };
}

// ------------------------------------------------------------------------------------------------
// The notice as one image, for the boot splash
// ------------------------------------------------------------------------------------------------
//
// The boot reads config.cfg off the card in VID_Init, before the renderer exists, so the splash
// draws the notice, and all it draws is textures. The image is put together here as
// DrawSaveNotice puts the notice on the menu canvas, one texel to a canvas pixel: the box from
// gfx/box_*.lmp, laid out as M_DrawTextBox lays it, and the lines in gfx.wad's conchars, through
// the built-in palette. Texels the pieces leave see-through stay at alpha 0, which the splash
// leaves out, so its plaque shows through them as the game would through the box's.

// Copies an 8-bit picture into the image at (x, y), clipped to it, leaving out the index
// `transparent`.
void BlitIndexed(u32 * const image, const u8 * const pixels, const int width, const int height, const int stride,
                 const int x, const int y, const int transparent)
{
    for (int row = 0; row < height; ++row)
    {
        for (int col = 0; col < width; ++col)
        {
            const int imageX = x + col;
            const int imageY = y + row;
            const int index  = pixels[(row * stride) + col];
            if (index != transparent && imageX >= 0 && imageX < kBoxWidth && imageY >= 0 && imageY < kBoxHeight)
            {
                image[(imageY * kBoxWidth) + imageX] = global_palette[index];
            }
        }
    }
}

// The box's pieces, gfx/box_<name>.lmp, each read once out of the game data: an LMP's width and
// height, then its texels, 255 see-through.
enum BoxPiece : int
{
    kTopLeft,
    kMiddleLeft,
    kBottomLeft,
    kTopMiddle,
    kMiddle,
    kMiddle2,
    kBottomMiddle,
    kTopRight,
    kMiddleRight,
    kBottomRight,
    kNumBoxPieces,
};

constexpr const char * kBoxPieceNames[kNumBoxPieces] = { "tl", "ml", "bl", "tm", "mm", "mm2", "bm", "tr", "mr", "br" };

struct LoadedPiece
{
    byte * file   = nullptr; // COM_LoadMallocFile's block; the texels start 8 bytes in.
    int    width  = 0;
    int    height = 0;
};

// False if the piece isn't there, or isn't an LMP that fits the box.
bool LoadBoxPiece(const BoxPiece piece, LoadedPiece & out)
{
    char path[MAX_QPATH];
    q_snprintf(path, sizeof(path), "gfx/box_%s.lmp", kBoxPieceNames[piece]);

    out.file = COM_LoadMallocFile(path, nullptr);
    if (out.file == nullptr)
    {
        return false;
    }

    const int * const header = static_cast<const int *>(static_cast<const void *>(out.file));
    out.width  = LittleLong(header[0]);
    out.height = LittleLong(header[1]);
    return out.width > 0 && out.height > 0 && out.width <= kBoxWidth && out.height <= kBoxHeight &&
           (8 + (out.width * out.height)) <= com_filesize;
}

void BlitBoxPiece(u32 * const image, const LoadedPiece & piece, const int x, const int y)
{
    BlitIndexed(image, piece.file + 8, piece.width, piece.height, piece.width, x, y, 255);
}

// M_DrawTextBox's layout, at (0, 0): the left column, the middle two characters at a time (its
// second row on is box_mm2, as M_DrawTextBox has it), then the right column.
void BlitBox(u32 * const image, const LoadedPiece (&pieces)[kNumBoxPieces])
{
    const int bottom = (kNumLines + 1) * kGlyph;

    BlitBoxPiece(image, pieces[kTopLeft], 0, 0);
    BlitBoxPiece(image, pieces[kBottomLeft], 0, bottom);
    for (int line = 0; line < kNumLines; ++line)
    {
        BlitBoxPiece(image, pieces[kMiddleLeft], 0, (line + 1) * kGlyph);
    }

    int x = kGlyph;
    for (int chars = kBoxChars; chars > 0; chars -= 2, x += 2 * kGlyph)
    {
        BlitBoxPiece(image, pieces[kTopMiddle], x, 0);
        BlitBoxPiece(image, pieces[kBottomMiddle], x, bottom);
        for (int line = 0; line < kNumLines; ++line)
        {
            BlitBoxPiece(image, pieces[(line == 0) ? kMiddle : kMiddle2], x, (line + 1) * kGlyph);
        }
    }

    BlitBoxPiece(image, pieces[kTopRight], x, 0);
    BlitBoxPiece(image, pieces[kBottomRight], x, bottom);
    for (int line = 0; line < kNumLines; ++line)
    {
        BlitBoxPiece(image, pieces[kMiddleRight], x, (line + 1) * kGlyph);
    }
}

// The image, kBoxWidth x kBoxHeight RGBA texels in a malloc block; null if a piece of it is
// missing.
u32 * ComposeNoticeImage()
{
    const u8 * const conchars = static_cast<const u8 *>(W_GetLumpName("conchars"));
    u32 * image = static_cast<u32 *>(std::calloc(static_cast<size_t>(kBoxWidth * kBoxHeight), sizeof(u32)));

    LoadedPiece pieces[kNumBoxPieces];
    bool loaded = (conchars != nullptr && image != nullptr);
    for (int i = 0; loaded && i < kNumBoxPieces; ++i)
    {
        loaded = LoadBoxPiece(static_cast<BoxPiece>(i), pieces[i]);
    }
    if (loaded)
    {
        BlitBox(image, pieces);
    }
    for (const LoadedPiece & piece : pieces)
    {
        std::free(piece.file);
    }

    if (!loaded)
    {
        std::free(image);
        return nullptr;
    }

    // The lines, centred as DrawSaveNotice centres them on the canvas, the box's corner there at
    // kBoxLeft; conchars are 16 to a row, 8x8 each, 0 see-through, the gold ones 128 on.
    const Lines notice = NoticeLines();
    const char * const * const lines = notice.line;
    for (int i = 0; i < kNumLines; ++i)
    {
        const int lineX = ((320 - (TextLength(lines[i]) * kGlyph)) / 2) - kBoxLeft;
        for (int c = 0; lines[i][c] != '\0'; ++c)
        {
            const int glyph = static_cast<unsigned char>(lines[i][c]) + ((i < kFirstWhiteLine) ? 128 : 0);
            const u8 * const pixels = conchars + ((glyph >> 4) * kGlyph * 128) + ((glyph & 15) * kGlyph);
            BlitIndexed(image, pixels, kGlyph, kGlyph, 128, lineX + (c * kGlyph), (i + 1) * kGlyph, 0);
        }
    }
    return image;
}

bool ShowOnSplash()
{
    u32 * const image = ComposeNoticeImage();
    const bool shown = (image != nullptr) && ps2::splash::ShowImage(image, kBoxWidth, kBoxHeight);
    std::free(image);
    return shown;
}

// ------------------------------------------------------------------------------------------------

// Puts a frame with the notice in it on screen before returning, wherever the game has got to:
//  - inside a frame, none: one frame can't be drawn inside another (the loading screen's
//    included, which would draw nothing and say so only to its own caller);
//  - with the loading plaque up, the loading screen's frame, the notice under the plaque;
//  - before SCR_Init, the boot splash, the notice over it: VID_Init reads config.cfg before the
//    renderer exists (between gs::Init and SCR_Init, nothing can be drawn at all);
//  - otherwise the game's frame, which GL_EndRendering draws the notice over and shows at once.
Where ShowFrame()
{
    if (ps2::rs::IsFrameOpen())
    {
        return Where::Nowhere;
    }
    if (ps2::loading::Redraw())
    {
        return Where::LoadingScreen;
    }
    if (!scr_initialized)
    {
        return ShowOnSplash() ? Where::Splash : Where::Nowhere;
    }

    // Host_Shutdown keeps Con_Printf from drawing while it writes config.cfg on the way out, with
    // what stands the screen still under the loading plaque. The notice draws anyway.
    const qboolean screenDisabled = scr_disabled_for_loading;
    scr_disabled_for_loading = false;
    SCR_UpdateScreen();
    scr_disabled_for_loading = screenDisabled;
    return Where::Game;
}

} // namespace

bool BeginSaveNotice(Device & device, const SaveAccess access)
{
    if (s_shown)
    {
        return true;
    }

    // The probe and the frame below hold the main thread up for some tens of milliseconds each, a
    // round trip to the device and the frame's vsyncs, while the sound feeder plays what the mixer
    // painted ahead (_snd_mixahead, 0.1 s). Topped up before each, it lasts until the music's fade
    // begins; without that, it ran out first in PCSX2, and the music stopped dead.
    S_ExtraUpdate();

    // A device that can't be used (no card in the slot, ...) fails the access at once: no notice
    // for that.
    if (!device.Probe())
    {
        return false;
    }

    s_shown  = true;
    s_access = access;
    s_kind   = device.Kind();

    // The game stands still from here until EndSaveNotice, with no frame to stop a motor.
    ps2::input::StopRumble();

    S_ExtraUpdate();
    s_where = ShowFrame();
    if (s_where == Where::Nowhere)
    {
        // A device is never read or written with nobody told. No path gets here now: the boot's
        // read of the card's config.cfg between gs::Init and SCR_Init only would, after a failed
        // first read, and quake.rc's "exec config.cfg" reads it again where the notice can be
        // drawn.
        s_shown = false;
        Con_DPrintf("Save notice: nothing can be drawn here, so the %s is left alone.\n", DeviceName(s_kind));
        return false;
    }
    s_shownMsec = ps2::sys::Milliseconds();

    ps2::audio::HoldMusic();
    return true;
}

void EndSaveNotice()
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
    Con_DPrintf("Save notice: down after %d ms.\n", ps2::sys::Milliseconds() - s_shownMsec);

    s_shown = false;

    // Off the loading screen or the splash at once. Over the game, the next frame is drawn
    // without it.
    if (!ps2::loading::Redraw() && s_where == Where::Splash)
    {
        ps2::splash::HideImage();
    }
    ps2::audio::ReleaseMusic();
}

bool SaveNoticeShown()
{
    return s_shown;
}

void DrawSaveNotice(const bool onLoadingScreen)
{
    if (!onLoadingScreen)
    {
        Draw_FadeScreen();
    }

    GL_SetCanvas(CANVAS_MENU);
    const int top = onLoadingScreen ? kBoxTopOnLoadingScreen : kBoxTopOverGame;
    M_DrawTextBox(kBoxLeft, top, kBoxChars, kNumLines);

    const Lines notice = NoticeLines();
    const char * const * const lines = notice.line;
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

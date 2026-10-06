/* ================================================================================================
 * File: draw.cpp
 * Brief: QuakeSpasm's 2D drawing seam (draw.h), in place of gl_draw.c: console text, pics, fills,
 *        the tiled border and the canvas transforms the console, status bar and menus are laid
 *        out in, drawn with ps2::rs's 2D primitives. Plus the engine_hooks.h stand-ins for the GL
 *        alpha, scissor and full-screen blend the engine used outside gl_draw.c.
 *
 *        Each pic is a qpic_t of our own carrying its texture's pointer where the pixels would
 *        start, as gl_draw.c kept its glpic_t there. WAD pics under 64x64 are packed into the
 *        scrap atlases, as QuakeSpasm does; every other pic is copied into a block the upload
 *        DMA can read (see MakeStandalone). The pics bypass TexMgr: nothing outside this file
 *        sees their textures, so the engine's texture manager has nothing to manage for them.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/renderer/draw.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/scrap_atlas.h"
#include "ps2/renderer/texture.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/heap.h"

#include <cmath>
#include <cstring>

extern "C" {
// gl_screen.c's 3D view rectangle, which the crosshair canvas centres on. No header has it:
// gl_draw.c declared it where it used it, too.
extern vrect_t scr_vrect;
}

namespace {

using ps2::tex::Texture;

// One console font glyph: conchars is a 16x16 grid of them.
constexpr int kGlyphSize = 8;

// The vertex colour of every textured 2D draw: under MODULATE, 128 leaves the texels as they are.
constexpr u8 kTexelColour[3] = { 128, 128, 128 };

// The console background's opacity: QuakeSpasm keeps it in the 2D module.
cvar_t s_conAlpha = ps2::MakeCvar("scr_conalpha", "0.5", CVAR_ARCHIVE);

// 0..1 opacity as the 0..255 byte the 2D primitives take, 255 being opaque.
u8 AlphaByte(const float alpha)
{
    const float clamped = (alpha <= 0.0f) ? 0.0f : (alpha >= 1.0f) ? 1.0f : alpha;
    return static_cast<u8>((clamped * 255.0f) + 0.5f);
}

// QuakeSpasm's CLAMP, minimum first, which gives the maximum when the two cross.
float Clamp(const float minVal, const float x, const float maxVal)
{
    return (x < minVal) ? minVal : (x > maxVal) ? maxVal : x;
}

// ------------------------------------------------------------------------------------------------
// Pics
// ------------------------------------------------------------------------------------------------

// A pic as the engine holds it - a qpic_t, which it lays things out from - with what it owns.
struct Pic
{
    qpic_t    pic;        // 'data' holds the texture's pointer
    Texture * texture;
    void *    pixels;     // the copy the texture samples; null when it is packed into a scrap
    u32       pixelBytes;
};
static_assert(sizeof(Texture *) <= sizeof(qpic_t::data), "A texture pointer must fit a qpic_t's data!");

const Texture & TextureOf(const qpic_t * pic)
{
    const Texture * texture = nullptr;
    std::memcpy(static_cast<void *>(&texture), pic->data, sizeof(texture));
    return *texture;
}

void SetPicTexture(Pic & pic, Texture & texture, const int width, const int height)
{
    const Texture * const pointer = &texture;
    std::memcpy(pic.pic.data, static_cast<const void *>(&pointer), sizeof(pointer));
    pic.pic.width  = width;
    pic.pic.height = height;
    pic.texture    = &texture;
}

// A pic with a texture of its own over a copy of 'src', which may not outlive it - a WAD lump, an
// LMP read into temporary memory. The upload DMA reads a texture straight out of EE RAM, in whole
// quadwords from a 16-byte boundary, so the copy starts on a cache line and its rows pad out to a
// multiple of 16 texels, transparent. The texture is that padded size; srcWidth/srcHeight keep
// the pic's own, which is all a draw samples. 'transparentZero' cuts index 0 out too, as
// QuakeSpasm's conchars palette did for the font's background.
void MakeStandalone(Pic & out, const char * name, const byte * src, const int width, const int height,
                    const bool transparentZero = false)
{
    const int paddedWidth = (width + 15) & ~15;
    const u32 bytes       = static_cast<u32>(paddedWidth * height);

    byte * const pixels = static_cast<byte *>(ps2::heap::AllocAligned(ps2::heap::MemAlign(64), bytes,
                                                                     ps2::heap::MemTag::TexImage));
    std::memset(pixels, 255, bytes);

    for (int y = 0; y < height; ++y)
    {
        const byte * const srcRow = src + (y * width);
        byte * const dstRow = pixels + (y * paddedWidth);
        for (int x = 0; x < width; ++x)
        {
            dstRow[x] = (transparentZero && srcRow[x] == 0) ? static_cast<byte>(255) : srcRow[x];
        }
    }

    // RGBA, so the palette's alpha reaches the alpha test, which drops index 255.
    Texture & texture = ps2::tex::Create(name, pixels, paddedWidth, height, ps2::tex::PixelFormat::Palette8,
                                         ps2::tex::TexComponents::RGBA, ps2::tex::ImageType::Pic);
    texture.srcWidth  = static_cast<s16>(width);
    texture.srcHeight = static_cast<s16>(height);

    out.pixels     = pixels;
    out.pixelBytes = bytes;
    SetPicTexture(out, texture, width, height);
}

// A small WAD pic packed into a scrap atlas. The atlas is what uploads and binds, so the pic's
// texture only records where in it the pic went; its pixels are the lump's, which nothing reads.
// False when every scrap is full.
bool MakeScrapped(Pic & out, const char * name, const qpic_t * lump)
{
    const Texture * atlas = nullptr;
    int atlasX = 0;
    int atlasY = 0;
    if (!ps2::scrap::TryPack(lump->data, lump->width, lump->height, &atlas, &atlasX, &atlasY))
    {
        return false;
    }

    Texture & texture = ps2::tex::Create(name, lump->data, lump->width, lump->height, ps2::tex::PixelFormat::Palette8,
                                         ps2::tex::TexComponents::RGBA, ps2::tex::ImageType::Pic);
    texture.atlas  = atlas;
    texture.atlasX = static_cast<s16>(atlasX);
    texture.atlasY = static_cast<s16>(atlasY);

    out.pixels     = nullptr;
    out.pixelBytes = 0;
    SetPicTexture(out, texture, lump->width, lump->height);
    return true;
}

void FreePic(Pic & pic)
{
    if (pic.texture != nullptr)
    {
        ps2::tex::Destroy(*pic.texture);
    }
    if (pic.pixels != nullptr)
    {
        ps2::heap::Free(pic.pixels, pic.pixelBytes, ps2::heap::MemTag::TexImage);
    }
    pic = {};
}

// Pics from gfx.wad: the status bar's, the disc, the border tile. Kept until a new game rereads
// the WAD, as QuakeSpasm's are.
constexpr int kMaxWadPics = 256;
Pic s_wadPics[kMaxWadPics];
int s_numWadPics = 0;

// Pics loaded by path (menus, the loading plaque, the console background), kept for the rest of
// the game the way QuakeSpasm's menu_cachepics are.
struct CachedPic
{
    char name[MAX_QPATH];
    Pic  pic;
};

constexpr int kMaxCachedPics = 128;
CachedPic s_cachedPics[kMaxCachedPics];
int       s_numCachedPics = 0;

// The font, and the tile SCR_TileClear borders a shrunk 3D view with.
Pic      s_conchars;
qpic_t * s_backtile = nullptr;

// QuakeSpasm's internal pics, from images compiled into gl_draw.c: the console's insert and
// overwrite cursors, and the stand-in for a pic gfx.wad doesn't have.
constexpr byte kPicOvrData[8][8] = {
    { 255, 255, 255, 255, 255, 255, 255, 255 },
    { 255,  15,  15,  15,  15,  15,  15, 255 },
    { 255,  15,  15,  15,  15,  15,  15,   2 },
    { 255,  15,  15,  15,  15,  15,  15,   2 },
    { 255,  15,  15,  15,  15,  15,  15,   2 },
    { 255,  15,  15,  15,  15,  15,  15,   2 },
    { 255,  15,  15,  15,  15,  15,  15,   2 },
    { 255, 255,   2,   2,   2,   2,   2,   2 },
};

constexpr byte kPicInsData[9][8] = {
    {  15,  15, 255, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    {  15,  15,   2, 255, 255, 255, 255, 255 },
    { 255,   2,   2, 255, 255, 255, 255, 255 },
};

constexpr byte kPicNulData[8][8] = {
    { 252, 252, 252, 252,   0,   0,   0,   0 },
    { 252, 252, 252, 252,   0,   0,   0,   0 },
    { 252, 252, 252, 252,   0,   0,   0,   0 },
    { 252, 252, 252, 252,   0,   0,   0,   0 },
    {   0,   0,   0,   0, 252, 252, 252, 252 },
    {   0,   0,   0,   0, 252, 252, 252, 252 },
    {   0,   0,   0,   0, 252, 252, 252, 252 },
    {   0,   0,   0,   0, 252, 252, 252, 252 },
};

Pic s_picIns;
Pic s_picOvr;
Pic s_picNul;

// The player picture the setup menu recolours (Draw_TransPicTranslate), as it was loaded: each
// change of colours translates it afresh into the pic's texture. QuakeSpasm's menuplyr_pixels.
byte  s_menuPlayerPixels[4096];
Pic * s_menuPlayerPic    = nullptr;
int   s_menuPlayerTop    = -2;
int   s_menuPlayerBottom = -2;

qpic_t * PicFromWad(const char * name)
{
    const qpic_t * const lump = static_cast<const qpic_t *>(W_GetLumpName(name));
    if (lump == nullptr)
    {
        return &s_picNul.pic;
    }

    if (s_numWadPics == kMaxWadPics)
    {
        Sys_Error("Draw_PicFromWad: more than %d pics", kMaxWadPics);
    }
    Pic & pic = s_wadPics[s_numWadPics++];

    char textureName[MAX_QPATH];
    q_snprintf(textureName, sizeof(textureName), "%s:%s", WADFILENAME, name);

    // The little ones into the scrap. Any that don't fit stand alone, where QuakeSpasm would
    // stop with "Scrap_AllocBlock: full".
    if (lump->width < 64 && lump->height < 64 && MakeScrapped(pic, textureName, lump))
    {
        return &pic.pic;
    }
    MakeStandalone(pic, textureName, lump->data, lump->width, lump->height);
    return &pic.pic;
}

qpic_t * CachePic(const char * path)
{
    for (int i = 0; i < s_numCachedPics; ++i)
    {
        if (std::strcmp(path, s_cachedPics[i].name) == 0)
        {
            return &s_cachedPics[i].pic.pic;
        }
    }

    if (s_numCachedPics == kMaxCachedPics)
    {
        Sys_Error("menu_numcachepics == MAX_CACHED_PICS");
    }

    qpic_t * const lump = static_cast<qpic_t *>(static_cast<void *>(COM_LoadTempFile(path, nullptr)));
    if (lump == nullptr)
    {
        Sys_Error("Draw_CachePic: failed to load %s", path);
    }
    SwapPic(lump);

    CachedPic & cached = s_cachedPics[s_numCachedPics++];
    q_strlcpy(cached.name, path, sizeof(cached.name));
    MakeStandalone(cached.pic, path, lump->data, lump->width, lump->height);

    // The one pic Draw_TransPicTranslate recolours keeps its pixels as they came.
    if (std::strcmp(path, "gfx/menuplyr.lmp") == 0 &&
        lump->width * lump->height <= static_cast<int>(sizeof(s_menuPlayerPixels)))
    {
        std::memcpy(s_menuPlayerPixels, lump->data, static_cast<size_t>(lump->width * lump->height));
        s_menuPlayerPic    = &cached.pic;
        s_menuPlayerTop    = -2;
        s_menuPlayerBottom = -2;
    }

    return &cached.pic.pic;
}

// QuakeSpasm's Draw_LoadPics: what is read out of gfx.wad up front.
void LoadPics()
{
    const byte * const conchars = static_cast<const byte *>(W_GetLumpName("conchars"));
    if (conchars == nullptr)
    {
        Sys_Error("Draw_LoadPics: couldn't load conchars");
    }
    MakeStandalone(s_conchars, WADFILENAME ":conchars", conchars, 128, 128, /*transparentZero=*/true);

    draw_disc  = PicFromWad("disc");
    s_backtile = PicFromWad("backtile");
}

// The setup menu's player in shirt and pants colours 'top' and 'bottom' (0 to 13): QuakeSpasm's
// TexMgr_ReloadImage translation, where a colour row in the palette's upper half runs backwards.
void TranslateMenuPlayer(const Pic & pic, const int top, const int bottom)
{
    byte translation[256];
    for (int i = 0; i < 256; ++i)
    {
        translation[i] = static_cast<byte>(i);
    }

    const int shirt = top * 16;
    const int pants = bottom * 16;
    for (int i = 0; i < 16; ++i)
    {
        translation[TOP_RANGE + i]    = static_cast<byte>((shirt < 128) ? (shirt + i) : (shirt + 15 - i));
        translation[BOTTOM_RANGE + i] = static_cast<byte>((pants < 128) ? (pants + i) : (pants + 15 - i));
    }

    const Texture & texture = *pic.texture;
    byte * const pixels = static_cast<byte *>(pic.pixels);
    for (int y = 0; y < texture.srcHeight; ++y)
    {
        for (int x = 0; x < texture.srcWidth; ++x)
        {
            pixels[(y * texture.width) + x] = translation[s_menuPlayerPixels[(y * texture.srcWidth) + x]];
        }
    }
    texture.MarkPixelsDirty(); // uploads again on its next bind
}

// ------------------------------------------------------------------------------------------------
// Canvases
// ------------------------------------------------------------------------------------------------

// Where the current canvas puts its units on screen: x * scaleX + originX, in pixels from the top
// left. gl_draw.c set each canvas up as a glOrtho box drawn into a glViewport rectangle; this is
// the scale and offset that pair amounts to.
struct CanvasTransform
{
    float scaleX, scaleY;
    float originX, originY;
};

CanvasTransform s_canvas      = { 1.0f, 1.0f, 0.0f, 0.0f };
canvastype      s_currentCanvas = CANVAS_NONE;

// glOrtho(left, right, bottom, top) into glViewport(vpX, vpY, vpWidth, vpHeight). The viewport
// is in GL's window coordinates, whose origin is the bottom left, as gl_draw.c computed it.
void SetCanvasTransform(const float left, const float right, const float bottom, const float top,
                        const int vpX, const int vpY, const int vpWidth, const int vpHeight)
{
    s_canvas.scaleX  = static_cast<float>(vpWidth) / (right - left);
    s_canvas.scaleY  = static_cast<float>(vpHeight) / (bottom - top);
    s_canvas.originX = static_cast<float>(vpX) - (left * s_canvas.scaleX);
    s_canvas.originY = static_cast<float>(glheight - vpY - vpHeight) - (top * s_canvas.scaleY);
}

// What glViewport made of gl_draw.c's float arguments: GLint, truncated.
int ViewportInt(const float v)
{
    return static_cast<int>(v);
}

struct ScreenRect
{
    int x, y, width, height;
};

int ToPixel(const float v)
{
    return static_cast<int>(std::floor(v + 0.5f));
}

// A rectangle in the current canvas's units, on screen. The edges are rounded rather than the
// corner and the size, so two rectangles that meet in canvas units still meet on screen.
ScreenRect ToScreen(const float x, const float y, const float width, const float height)
{
    const int x0 = ToPixel(s_canvas.originX + (x * s_canvas.scaleX));
    const int y0 = ToPixel(s_canvas.originY + (y * s_canvas.scaleY));
    const int x1 = ToPixel(s_canvas.originX + ((x + width) * s_canvas.scaleX));
    const int y1 = ToPixel(s_canvas.originY + ((y + height) * s_canvas.scaleY));
    return { x0, y0, x1 - x0, y1 - y0 };
}

// ------------------------------------------------------------------------------------------------
// Drawing, in canvas units
// ------------------------------------------------------------------------------------------------

void DrawGlyph(const int x, const int y, const int num)
{
    const int row = (num >> 4) * kGlyphSize;
    const int col = (num & 15) * kGlyphSize;

    const ScreenRect r = ToScreen(static_cast<float>(x), static_cast<float>(y), kGlyphSize, kGlyphSize);
    ps2::rs::DrawTexturedRect(*s_conchars.texture, r.x, r.y, r.width, r.height,
                              col, row, col + kGlyphSize, row + kGlyphSize, kTexelColour);
}

// The pic stretched over its qpic_t's width and height, which the console background sets to
// the console's size before drawing: the texture is sampled over the pic's real size regardless.
void DrawPic(const int x, const int y, const qpic_t * pic, const u8 alpha)
{
    const Texture & texture = TextureOf(pic);
    const ScreenRect r = ToScreen(static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(pic->width), static_cast<float>(pic->height));
    ps2::rs::DrawTexturedRect(texture, r.x, r.y, r.width, r.height,
                              0, 0, texture.srcWidth, texture.srcHeight, kTexelColour, alpha);
}

void FillRect(const float x, const float y, const float width, const float height,
              const u8 r, const u8 g, const u8 b, const u8 a)
{
    const ScreenRect rect = ToScreen(x, y, width, height);
    ps2::rs::FillRect(rect.x, rect.y, rect.width, rect.height, r, g, b, a);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Backend
// ------------------------------------------------------------------------------------------------

const ps2::tex::Texture & ps2::draw::Conchars()
{
    return *s_conchars.texture;
}

extern "C" {

// ------------------------------------------------------------------------------------------------
// Engine-visible 2D state
// ------------------------------------------------------------------------------------------------

qpic_t * draw_disc = nullptr;
qpic_t * pic_ins   = nullptr;
qpic_t * pic_ovr   = nullptr;

// ------------------------------------------------------------------------------------------------
// Pics
// ------------------------------------------------------------------------------------------------

qpic_t * Draw_PicFromWad(const char * name)
{
    return PicFromWad(name);
}

qpic_t * Draw_CachePic(const char * path)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui); // the menus look theirs up every frame
    return CachePic(path);
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Draw_Init()
{
    Cvar_RegisterVariable(&s_conAlpha);

    MakeStandalone(s_picIns, "ins", &kPicInsData[0][0], 8, 9);
    MakeStandalone(s_picOvr, "ovr", &kPicOvrData[0][0], 8, 8);
    MakeStandalone(s_picNul, "nul", &kPicNulData[0][0], 8, 8);
    pic_ins = &s_picIns.pic;
    pic_ovr = &s_picOvr.pic;

    LoadPics();
}

// A new game directory may bring its own gfx.wad, so the WAD pics are reread and the path cache
// forgets what it held, as in QuakeSpasm. ('game' needs the registered version.)
void Draw_NewGame()
{
    // A frame left drawing may still be sampling the pics about to go.
    ps2::rs::FinishFrameInFlight();

    for (int i = 0; i < s_numWadPics; ++i)
    {
        FreePic(s_wadPics[i]);
    }
    s_numWadPics = 0;
    FreePic(s_conchars);
    ps2::scrap::Reset();

    W_LoadWadFile();
    LoadPics();
    SCR_LoadPics();
    Sbar_LoadPics();

    for (int i = 0; i < s_numCachedPics; ++i)
    {
        FreePic(s_cachedPics[i].pic);
        s_cachedPics[i].name[0] = '\0';
    }
    s_numCachedPics = 0;
    s_menuPlayerPic = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------------
//
// Every entry point charges the "Ui" event, so the overlay and the frame log show one figure for
// the whole 2D pass the engine draws. None of them nest: they share the helpers above instead of
// calling each other.

void Draw_Character(int x, int y, int num)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    if (y <= -8)
    {
        return; // totally off screen
    }

    num &= 255;
    if (num == 32)
    {
        return; // don't waste draws on spaces
    }

    DrawGlyph(x, y, num);
}

void Draw_String(int x, const int y, const char * str)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    if (y <= -8)
    {
        return; // totally off screen
    }

    for (; *str != '\0'; ++str, x += 8)
    {
        if (*str != 32) // don't waste draws on spaces
        {
            DrawGlyph(x, y, static_cast<unsigned char>(*str));
        }
    }
}

void Draw_Pic(int x, int y, qpic_t * pic)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);
    DrawPic(x, y, pic, 255);
}

// Only the setup menu's player picture is ever translated.
void Draw_TransPicTranslate(int x, int y, qpic_t * pic, int top, int bottom)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    if (s_menuPlayerPic != nullptr && pic == &s_menuPlayerPic->pic &&
        (top != s_menuPlayerTop || bottom != s_menuPlayerBottom))
    {
        s_menuPlayerTop    = top;
        s_menuPlayerBottom = bottom;
        TranslateMenuPlayer(*s_menuPlayerPic, top, bottom);
    }
    DrawPic(x, y, pic, 255);
}

void Draw_ConsoleBackground()
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    qpic_t * const pic = CachePic("gfx/conback.lmp");
    pic->width  = vid.conwidth;
    pic->height = vid.conheight;

    const float alpha = con_forcedup ? 1.0f : s_conAlpha.value;

    GL_SetCanvas(CANVAS_CONSOLE); // in case this is called from weird places

    if (alpha > 0.0f)
    {
        DrawPic(0, 0, pic, AlphaByte(alpha));
    }
}

// Repeats the 64x64 border tile over a rectangle, texels addressed in screen space: the tile
// wraps (REPEAT, set up by gs::Init) where its texel coordinates pass 64.
void Draw_TileClear(int x, int y, int w, int h)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    const ScreenRect r = ToScreen(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                  static_cast<float>(h));
    ps2::rs::DrawTexturedRect(TextureOf(s_backtile), r.x, r.y, r.width, r.height, x, y, x + w, y + h,
                              kTexelColour);
}

// A box of one palette colour, at 'alpha' opacity.
void Draw_Fill(int x, int y, int w, int h, int c, float alpha)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    const u32 rgba = d_8to24table[c & 0xFF];
    FillRect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h),
             static_cast<u8>(rgba & 0xFFu), static_cast<u8>((rgba >> 8) & 0xFFu),
             static_cast<u8>((rgba >> 16) & 0xFFu), AlphaByte(alpha));
}

// Darkens the whole screen under a menu.
void Draw_FadeScreen()
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    GL_SetCanvas(CANVAS_DEFAULT);
    FillRect(0.0f, 0.0f, static_cast<float>(glwidth), static_cast<float>(glheight), 0, 0, 0, AlphaByte(0.5f));

    Sbar_Changed();
}

void GL_SetCanvas(canvastype newcanvas)
{
    if (newcanvas == s_currentCanvas)
    {
        return;
    }
    s_currentCanvas = newcanvas;

    const float width  = static_cast<float>(glwidth);
    const float height = static_cast<float>(glheight);
    float s = 1.0f;

    switch (newcanvas)
    {
    case CANVAS_DEFAULT:
        SetCanvasTransform(0.0f, width, height, 0.0f, glx, gly, glwidth, glheight);
        break;

    case CANVAS_CONSOLE:
    {
        const float conHeight = static_cast<float>(vid.conheight);
        const int   lines     = static_cast<int>(conHeight - (scr_con_current * conHeight / height));
        SetCanvasTransform(0.0f, static_cast<float>(vid.conwidth), conHeight + static_cast<float>(lines),
                           static_cast<float>(lines), glx, gly, glwidth, glheight);
        break;
    }

    case CANVAS_MENU:
        s = std::fmin(width / 320.0f, height / 200.0f);
        s = Clamp(1.0f, scr_menuscale.value, s);
        // ericw -- doubled width to 640 to accommodate long keybindings
        SetCanvasTransform(0.0f, 640.0f, 200.0f, 0.0f,
                           ViewportInt(static_cast<float>(glx) + ((width - (320.0f * s)) / 2.0f)),
                           ViewportInt(static_cast<float>(gly) + ((height - (200.0f * s)) / 2.0f)),
                           ViewportInt(640.0f * s), ViewportInt(200.0f * s));
        break;

    case CANVAS_SBAR:
        s = Clamp(1.0f, scr_sbarscale.value, width / 320.0f);
        if (cl.gametype == GAME_DEATHMATCH)
        {
            SetCanvasTransform(0.0f, width / s, 48.0f, 0.0f, glx, gly, glwidth, ViewportInt(48.0f * s));
        }
        else
        {
            SetCanvasTransform(0.0f, 320.0f, 48.0f, 0.0f,
                               ViewportInt(static_cast<float>(glx) + ((width - (320.0f * s)) / 2.0f)), gly,
                               ViewportInt(320.0f * s), ViewportInt(48.0f * s));
        }
        break;

    case CANVAS_WARPIMAGE:
        SetCanvasTransform(0.0f, 128.0f, 0.0f, 128.0f, glx, gly + glheight - gl_warpimagesize,
                           gl_warpimagesize, gl_warpimagesize);
        break;

    case CANVAS_CROSSHAIR: // 0,0 is the centre of the 3D view
        s = Clamp(1.0f, scr_crosshairscale.value, 10.0f);
        SetCanvasTransform(static_cast<float>(scr_vrect.width / -2) / s, static_cast<float>(scr_vrect.width / 2) / s,
                           static_cast<float>(scr_vrect.height / 2) / s, static_cast<float>(scr_vrect.height / -2) / s,
                           scr_vrect.x, glheight - scr_vrect.y - scr_vrect.height,
                           scr_vrect.width & ~1, scr_vrect.height & ~1);
        break;

    case CANVAS_BOTTOMLEFT: // used by devstats
        s = width / static_cast<float>(vid.conwidth); // use console scale
        SetCanvasTransform(0.0f, 320.0f, 200.0f, 0.0f, glx, gly, ViewportInt(320.0f * s), ViewportInt(200.0f * s));
        break;

    case CANVAS_BOTTOMRIGHT: // used by fps/clock
        s = width / static_cast<float>(vid.conwidth); // use console scale
        SetCanvasTransform(0.0f, 320.0f, 200.0f, 0.0f, ViewportInt(static_cast<float>(glx) + width - (320.0f * s)), gly,
                           ViewportInt(320.0f * s), ViewportInt(200.0f * s));
        break;

    case CANVAS_TOPRIGHT: // used by disc
        SetCanvasTransform(0.0f, 320.0f, 200.0f, 0.0f, ViewportInt(static_cast<float>(glx) + width - (320.0f * s)),
                           ViewportInt(static_cast<float>(gly) + height - (200.0f * s)),
                           ViewportInt(320.0f * s), ViewportInt(200.0f * s));
        break;

    default:
        Sys_Error("GL_SetCanvas: bad canvas type");
    }
}

void GL_Set2D()
{
    s_currentCanvas = CANVAS_INVALID;
    GL_SetCanvas(CANVAS_DEFAULT);
}

// ------------------------------------------------------------------------------------------------
// engine_hooks.h
// ------------------------------------------------------------------------------------------------

void PS2_DrawPicAlpha(int x, int y, qpic_t * pic, float alpha)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);
    DrawPic(x, y, pic, AlphaByte(alpha));
}

void PS2_SetScissor2D(int x, int y, int width, int height)
{
    ps2::rs::SetScissor2D(x, y, width, height);
}

void PS2_ResetScissor2D()
{
    ps2::rs::ResetScissor2D();
}

// Over the 3D view, whatever canvas is current: GL drew it into the view's viewport.
void PS2_DrawPolyBlend(const float rgba[4])
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Ui);

    const vrect_t & view = r_refdef.vrect;
    ps2::rs::FillRect(view.x, view.y, view.width, view.height,
                      AlphaByte(rgba[0]), AlphaByte(rgba[1]), AlphaByte(rgba[2]), AlphaByte(rgba[3]));
}

} // extern "C"

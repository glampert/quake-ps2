/* ================================================================================================
 * File: draw.cpp
 * Brief: QuakeSpasm's 2D drawing seam (draw.h): console characters, pics, fills and the canvas
 *        transforms the status bar and menus are laid out in, plus the engine_hooks.h stand-ins
 *        for the GL alpha and scissor the status bar used.
 *
 *        Nothing is drawn yet. The pics are real, though - read from gfx.wad or the game's LMP
 *        files - because the menus and the status bar lay themselves out from their sizes.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"

#include <cstring>

namespace {

// Pics loaded by path (menus, the loading plaque), kept for the rest of the game the way
// QuakeSpasm's menu_cachepics are. Only the header matters until the pics are drawn.
struct CachedPic
{
    char   name[MAX_QPATH];
    qpic_t header;
};

constexpr int kMaxCachedPics = 128;
CachedPic s_cachedPics[kMaxCachedPics];
int       s_numCachedPics = 0;

// The console's cursor glyphs and the stand-in for a pic missing from gfx.wad, which QuakeSpasm
// builds from images compiled into gl_draw.c. Their sizes are what the engine reads.
qpic_t s_picIns = { 8, 9, {} };
qpic_t s_picOvr = { 8, 8, {} };
qpic_t s_picNul = { 8, 8, {} };

// The console background's opacity, which QuakeSpasm's 2D module owns.
cvar_t s_conAlpha = ps2::MakeCvar("scr_conalpha", "0.5", CVAR_ARCHIVE);

void LoadWadPics()
{
    draw_disc = Draw_PicFromWad("disc");
}

} // namespace

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
    qpic_t * const pic = static_cast<qpic_t *>(W_GetLumpName(name));
    return (pic != nullptr) ? pic : &s_picNul;
}

qpic_t * Draw_CachePic(const char * path)
{
    for (int i = 0; i < s_numCachedPics; ++i)
    {
        if (std::strcmp(path, s_cachedPics[i].name) == 0)
        {
            return &s_cachedPics[i].header;
        }
    }

    if (s_numCachedPics == kMaxCachedPics)
    {
        Sys_Error("Draw_CachePic: more than %d pics", kMaxCachedPics);
    }

    const void * const file = COM_LoadTempFile(path, nullptr);
    const qpic_t * const lump = static_cast<const qpic_t *>(file);
    if (lump == nullptr)
    {
        Sys_Error("Draw_CachePic: failed to load %s", path);
    }

    CachedPic & cached = s_cachedPics[s_numCachedPics++];
    q_strlcpy(cached.name, path, sizeof(cached.name));
    cached.header.width  = LittleLong(lump->width);
    cached.header.height = LittleLong(lump->height);
    return &cached.header;
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Draw_Init()
{
    Cvar_RegisterVariable(&s_conAlpha);

    pic_ins = &s_picIns;
    pic_ovr = &s_picOvr;

    LoadWadPics();
}

// A new game directory may bring its own gfx.wad, so the pics taken from the old one are reread,
// and the path cache forgets what it held - as QuakeSpasm does.
void Draw_NewGame()
{
    W_LoadWadFile();
    LoadWadPics();
    SCR_LoadPics();
    Sbar_LoadPics();

    s_numCachedPics = 0;
}

// ------------------------------------------------------------------------------------------------
// Drawing: nothing reaches the screen yet
// ------------------------------------------------------------------------------------------------

void Draw_Character(int x, int y, int num)                                  { (void)x; (void)y; (void)num; }
void Draw_String(int x, int y, const char * str)                            { (void)x; (void)y; (void)str; }
void Draw_Pic(int x, int y, qpic_t * pic)                                   { (void)x; (void)y; (void)pic; }
void Draw_TransPicTranslate(int x, int y, qpic_t * pic, int top, int bottom) { (void)x; (void)y; (void)pic; (void)top; (void)bottom; }
void Draw_ConsoleBackground()                                               {}
void Draw_TileClear(int x, int y, int w, int h)                             { (void)x; (void)y; (void)w; (void)h; }
void Draw_Fill(int x, int y, int w, int h, int c, float alpha)              { (void)x; (void)y; (void)w; (void)h; (void)c; (void)alpha; }
void Draw_FadeScreen()                                                      {}

void GL_SetCanvas(canvastype newcanvas) { (void)newcanvas; }
void GL_Set2D() {}

// engine_hooks.h:
void PS2_DrawPicAlpha(int x, int y, qpic_t * pic, float alpha)     { (void)x; (void)y; (void)pic; (void)alpha; }
void PS2_SetScissor2D(int x, int y, int width, int height)        { (void)x; (void)y; (void)width; (void)height; }
void PS2_ResetScissor2D()                                         {}

} // extern "C"

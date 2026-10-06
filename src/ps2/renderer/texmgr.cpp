/* ================================================================================================
 * File: texmgr.cpp
 * Brief: QuakeSpasm's texture manager seam (gl_texmgr.h). The engine loads every texture through
 *        TexMgr_LoadImage - world and model textures from gl_model.c, pics from the 2D code - and
 *        keeps the gltexture_t it gets back.
 *
 *        Nothing reaches the GS yet: each texture is a record of what was asked for (name, size,
 *        owner, flags), so the engine's bookkeeping - lookups, per-model frees, the new-game
 *        purge - behaves as it will once the records point at PS2 textures.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <cstring>

#include <tamtypes.h>

namespace {

// Every live texture, newest first, through gltexture_t's own 'next' link.
gltexture_t * s_textures = nullptr;

gltexture_t * NewTexture()
{
    void * const memory = ps2::heap::Alloc(sizeof(gltexture_t), ps2::heap::MemTag::TexImage);
    std::memset(memory, 0, sizeof(gltexture_t));

    gltexture_t * const texture = static_cast<gltexture_t *>(memory);
    texture->next = s_textures;
    s_textures = texture;
    return texture;
}

// Unlinks and frees every texture 'shouldFree' accepts.
template<typename Predicate>
void FreeTexturesWhere(Predicate shouldFree)
{
    gltexture_t ** link = &s_textures;
    while (*link != nullptr)
    {
        gltexture_t * const texture = *link;
        if (shouldFree(*texture))
        {
            *link = texture->next;
            ps2::heap::Free(texture, sizeof(gltexture_t), ps2::heap::MemTag::TexImage);
        }
        else
        {
            link = &texture->next;
        }
    }
}

// gfx/palette.lmp as QuakeSpasm keeps it: one RGBA word per index, little-endian, with index 255
// transparent. gl_model.c reads it when it flood-fills a model skin's background.
void LoadPalette()
{
    const void * const file = COM_LoadTempFile("gfx/palette.lmp", nullptr);
    if (file == nullptr)
    {
        Sys_Error("Couldn't load gfx/palette.lmp");
    }

    const byte * const rgb = static_cast<const byte *>(file);
    for (int i = 0; i < 256; ++i)
    {
        const u32 r = rgb[(i * 3) + 0];
        const u32 g = rgb[(i * 3) + 1];
        const u32 b = rgb[(i * 3) + 2];
        const u32 a = (i == 255) ? 0u : 255u;
        d_8to24table[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// Engine-visible texture state
// ------------------------------------------------------------------------------------------------

gltexture_t * notexture   = nullptr;
gltexture_t * nulltexture = nullptr;

unsigned int d_8to24table[256];

// QuakeSpasm only generates mipmaps for its warp images when the GL driver can; the model loader
// asks through this pointer, and on the PS2 there is no driver to ask.
QS_PFNGENERATEMIPMAP GL_GenerateMipmap = nullptr;

// ------------------------------------------------------------------------------------------------
// TexMgr_*
// ------------------------------------------------------------------------------------------------

void TexMgr_Init()
{
    LoadPalette();

    notexture   = TexMgr_LoadImage(nullptr, "notexture",   2, 2, SRC_RGBA, nullptr, "", 0, TEXPREF_PERSIST);
    nulltexture = TexMgr_LoadImage(nullptr, "nulltexture", 2, 2, SRC_RGBA, nullptr, "", 0, TEXPREF_PERSIST);
}

gltexture_t * TexMgr_LoadImage(qmodel_t * owner, const char * name, int width, int height,
                               enum srcformat format, byte * data, const char * source_file,
                               src_offset_t source_offset, unsigned flags)
{
    (void)data;

    // TEXPREF_OVERWRITE reloads a texture in place (a player skin taking new colours), so the
    // engine's pointer to it stays good. Anything else is a new texture.
    gltexture_t * texture = nullptr;
    if ((flags & TEXPREF_OVERWRITE) != 0)
    {
        for (gltexture_t * t = s_textures; t != nullptr; t = t->next)
        {
            if (t->owner == owner && std::strcmp(t->name, name) == 0)
            {
                texture = t;
                break;
            }
        }
    }
    if (texture == nullptr)
    {
        texture = NewTexture();
    }

    texture->owner = owner;
    q_strlcpy(texture->name, name, sizeof(texture->name));
    texture->width         = static_cast<unsigned int>(width);
    texture->height        = static_cast<unsigned int>(height);
    texture->flags         = flags;
    q_strlcpy(texture->source_file, source_file, sizeof(texture->source_file));
    texture->source_offset = source_offset;
    texture->source_format = format;
    texture->source_width  = static_cast<unsigned int>(width);
    texture->source_height = static_cast<unsigned int>(height);
    texture->shirt         = -1;
    texture->pants         = -1;
    return texture;
}

void TexMgr_FreeTexturesForOwner(qmodel_t * owner)
{
    FreeTexturesWhere([owner](const gltexture_t & t) { return t.owner == owner; });
}

// A new game (the 'game' command) drops every texture not marked to persist.
void TexMgr_NewGame()
{
    FreeTexturesWhere([](const gltexture_t & t) { return (t.flags & TEXPREF_PERSIST) == 0; });
    LoadPalette();
}

// QuakeSpasm pads a non-power-of-two texture for drivers that need it, which none here does.
int TexMgr_PadConditional(int s)
{
    return s;
}

} // extern "C"

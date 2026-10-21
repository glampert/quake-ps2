/* ================================================================================================
 * File: imgdump.cpp
 * Created on: 26/10/15
 * Brief: Dumps Quake's 8-bit pics into C++ byte arrays, to be compiled into the ELF. The built-ins
 *        under src/ps2/builtin/ come from it: the QUAKE plaque down the left of the menus, which
 *        the boot splash shows before any game data has been found, the palette it draws
 *        through, and the main menu's About item, from the .lmp make_menu_about.py makes.
 *
 *          imgdump [--format pal|rgba32|rgb16] [--palette <palette.lmp>] [--out <dir>] <pic.lmp>...
 *
 *            --format F   pal (the default) keeps the 8-bit palette indices as they are: the PS2
 *                         uploads them as PSMT8 textures sampling the palette's CLUT. rgba32 and
 *                         rgb16 expand them through --palette, which they need, into RGBA 8:8:8:8
 *                         or RGB 5:5:5:1 texels.
 *            --palette P  gfx/palette.lmp, the 256 colours every pic's indices are into. It is
 *                         written to palette.cpp as global_palette.
 *            --out D      where the .cpp files go (default: the current directory).
 *
 *        Each <name>.lmp, a pic as the gfx/ lumps hold one (a qpic_t: its width and height, then
 *        the indices, row by row), becomes <name>.cpp, defining <name>_width, <name>_height,
 *        <name>_size_bytes and <name>_data, which builtin.h declares. The indices are the file's,
 *        as Draw_CachePic reads them, so a built-in holds the same pixels as the file it stands in
 *        for. To regenerate them from an unpacked pak0 (build/tools/unpak pak0.pak):
 *
 *          imgdump --palette pak0/gfx/palette.lmp --out src/ps2/builtin pak0/gfx/qplaque.lmp
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------------------------------------
// Quake's pics and palette
// ------------------------------------------------------------------------------------------------

constexpr int kPicHeaderBytes   = 8;   // a qpic_t's width and height, little-endian 32-bit ints
constexpr int kMaxPicSize       = 4096; // past anything id's data holds, to catch a file that isn't a pic
constexpr int kPaletteEntries   = 256;
constexpr int kPaletteBytes     = kPaletteEntries * 3;
constexpr int kTransparentIndex = 255; // Quake's see-through palette entry

enum class Format
{
    Pal,
    Rgba32,
    Rgb16,
};

struct Image
{
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> indices; // width * height palette indices, row by row
};

using Palette = std::uint32_t[kPaletteEntries]; // 0xAABBGGRR, index 255 transparent

bool Fail(const char * format, const char * arg = "")
{
    std::fprintf(stderr, "imgdump: error: ");
    std::fprintf(stderr, format, arg);
    std::fprintf(stderr, "\n");
    return false;
}

bool ReadWholeFile(const char * path, std::vector<std::uint8_t> & out)
{
    std::FILE * file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return Fail("cannot open '%s'", path);
    }

    bool ok = (std::fseek(file, 0, SEEK_END) == 0);
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok)
    {
        out.resize(static_cast<size_t>(size));
        ok = (size == 0) || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok ? true : Fail("cannot read '%s'", path);
}

int ReadS32(const std::uint8_t * bytes)
{
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

// A pic lump as the game reads one (Draw_CachePic, SwapPic): the header's width and height, then
// exactly that many indices. Anything else is not a pic.
bool LoadPic(const char * path, Image & image)
{
    std::vector<std::uint8_t> file;
    if (!ReadWholeFile(path, file))
    {
        return false;
    }
    if (file.size() < static_cast<size_t>(kPicHeaderBytes))
    {
        return Fail("'%s' is too short to be a pic", path);
    }

    const int width  = ReadS32(file.data());
    const int height = ReadS32(file.data() + 4);
    if (width <= 0 || height <= 0 || width > kMaxPicSize || height > kMaxPicSize ||
        file.size() != static_cast<size_t>(kPicHeaderBytes) + (static_cast<size_t>(width) * static_cast<size_t>(height)))
    {
        return Fail("'%s' is not a pic: its size doesn't match the width and height it starts with", path);
    }

    image.width  = width;
    image.height = height;
    image.indices.assign(file.begin() + kPicHeaderBytes, file.end());
    return true;
}

// gfx/palette.lmp: 256 RGB triplets and nothing else. Opaque, but for index 255, which every
// see-through texel is.
bool LoadPalette(const char * path, Palette & palette)
{
    std::vector<std::uint8_t> file;
    if (!ReadWholeFile(path, file))
    {
        return false;
    }
    if (file.size() != static_cast<size_t>(kPaletteBytes))
    {
        return Fail("'%s' is not a palette: it isn't 768 bytes long", path);
    }

    for (int i = 0; i < kPaletteEntries; ++i)
    {
        const std::uint32_t r = file[static_cast<size_t>(i * 3) + 0];
        const std::uint32_t g = file[static_cast<size_t>(i * 3) + 1];
        const std::uint32_t b = file[static_cast<size_t>(i * 3) + 2];
        palette[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    palette[kTransparentIndex] &= 0x00FFFFFFu;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Expansion through the palette
// ------------------------------------------------------------------------------------------------

// RGBA 8:8:8:8 as the palette's own bytes. A transparent texel takes the colour of an opaque
// neighbour, so bilinear filtering has no dark fringe to blend in at the edge of a cut-out.
std::vector<std::uint8_t> ExpandRgba32(const Image & image, const Palette & palette)
{
    const int numTexels = image.width * image.height;
    const int w = image.width;
    const std::uint8_t * in = image.indices.data();
    std::vector<std::uint8_t> out(static_cast<size_t>(numTexels) * 4);

    for (int i = 0; i < numTexels; ++i)
    {
        std::uint32_t texel = palette[in[i]];

        if (in[i] == kTransparentIndex)
        {
            int neighbour = 0;
            if (i > w && in[i - w] != kTransparentIndex)
            {
                neighbour = in[i - w];
            }
            else if (i < numTexels - w && in[i + w] != kTransparentIndex)
            {
                neighbour = in[i + w];
            }
            else if (i > 0 && in[i - 1] != kTransparentIndex)
            {
                neighbour = in[i - 1];
            }
            else if (i < numTexels - 1 && in[i + 1] != kTransparentIndex)
            {
                neighbour = in[i + 1];
            }
            texel = (texel & 0xFF000000u) | (palette[neighbour] & 0x00FFFFFFu);
        }

        std::memcpy(&out[static_cast<size_t>(i) * 4], &texel, sizeof(texel));
    }
    return out;
}

// RGB 5:5:5 plus the top bit of the palette alpha, the GS's PSMCT16 layout, little-endian.
std::vector<std::uint8_t> ExpandRgb16(const Image & image, const Palette & palette)
{
    const int numTexels = image.width * image.height;
    std::vector<std::uint8_t> out(static_cast<size_t>(numTexels) * 2);

    for (int i = 0; i < numTexels; ++i)
    {
        const std::uint32_t colour = palette[image.indices[static_cast<size_t>(i)]];
        const std::uint32_t r = colour & 0xFFu;
        const std::uint32_t g = (colour >> 8) & 0xFFu;
        const std::uint32_t b = (colour >> 16) & 0xFFu;
        const std::uint32_t a = (colour >> 24) & 0xFFu;

        const std::uint32_t texel = ((a & 0x1u) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
        out[static_cast<size_t>(i) * 2 + 0] = static_cast<std::uint8_t>(texel & 0xFFu);
        out[static_cast<size_t>(i) * 2 + 1] = static_cast<std::uint8_t>(texel >> 8);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Output
// ------------------------------------------------------------------------------------------------

bool CloseOutput(std::FILE * file, const std::string & path)
{
    const bool written = (std::ferror(file) == 0);
    const bool closed  = (std::fclose(file) == 0);
    return (written && closed) ? true : Fail("cannot write '%s'", path.c_str());
}

bool WritePalette(const std::string & path, const Palette & palette)
{
    std::FILE * file = std::fopen(path.c_str(), "wt");
    if (file == nullptr)
    {
        return Fail("cannot create '%s'", path.c_str());
    }

    std::fprintf(file, "// File generated by imgdump\n#include \"builtin.h\"\n\n");
    std::fprintf(file, "alignas(16) const unsigned int global_palette[256] = {\n    ");
    int column = 0;
    for (int i = 0; i < kPaletteEntries; ++i)
    {
        std::fprintf(file, "0x%08X", palette[i]);
        if (i != kPaletteEntries - 1)
        {
            std::fprintf(file, ", ");
        }
        if (++column > 4)
        {
            std::fprintf(file, "\n    ");
            column = 0;
        }
    }
    std::fprintf(file, "\n};\n\n");
    return CloseOutput(file, path);
}

bool WriteImage(const std::string & path, const std::string & name, const char * contents,
                const Image & image, const std::vector<std::uint8_t> & bytes)
{
    std::FILE * file = std::fopen(path.c_str(), "wt");
    if (file == nullptr)
    {
        return Fail("cannot create '%s'", path.c_str());
    }

    const int numBytes = static_cast<int>(bytes.size());
    std::fprintf(file, "// File generated by imgdump (%s)\n#include \"builtin.h\"\n\n", contents);
    std::fprintf(file, "const int %s_width  = %d;\n", name.c_str(), image.width);
    std::fprintf(file, "const int %s_height = %d;\n", name.c_str(), image.height);
    std::fprintf(file, "const int %s_size_bytes = %d;\n", name.c_str(), numBytes);
    std::fprintf(file, "alignas(16) const unsigned char %s_data[] = {\n    ", name.c_str());
    int column = 0;
    for (int i = 0; i < numBytes; ++i)
    {
        std::fprintf(file, "0x%02X", bytes[static_cast<size_t>(i)]);
        if (i != numBytes - 1)
        {
            std::fprintf(file, ", ");
        }
        if (++column > 14)
        {
            std::fprintf(file, "\n    ");
            column = 0;
        }
    }
    std::fprintf(file, "\n};\n\n");
    return CloseOutput(file, path);
}

// The file name without its folders or extension, made a C identifier: "gfx/qplaque.lmp" names
// the qplaque_* symbols, in qplaque.cpp.
std::string SymbolName(const char * path)
{
    const char * base = path;
    for (const char * p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }

    std::string name(base);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot != 0)
    {
        name.resize(dot);
    }

    for (char & c : name)
    {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        c = valid ? c : '_';
    }
    if (name.empty() || (name[0] >= '0' && name[0] <= '9'))
    {
        name.insert(0, 1, '_');
    }
    return name;
}

// ------------------------------------------------------------------------------------------------
// Command line
// ------------------------------------------------------------------------------------------------

struct Options
{
    Format                     format      = Format::Pal;
    const char *               palettePath = nullptr;
    std::string                outDir      = ".";
    std::vector<const char *>  images;
};

void PrintUsage()
{
    std::fprintf(stderr,
                 "usage: imgdump [--format pal|rgba32|rgb16] [--palette <palette.lmp>] [--out <dir>] <pic.lmp>...\n"
                 "  --format F   pal (default): 8-bit palette indices; rgba32/rgb16: expanded through --palette\n"
                 "  --palette P  gfx/palette.lmp: written to palette.cpp, and what rgba32/rgb16 expand through\n"
                 "  --out D      directory for the .cpp files (default: the current one)\n"
                 "Each <name>.lmp becomes <name>.cpp, defining <name>_width/_height/_size_bytes/_data.\n");
}

bool ParseArgs(const int argc, char ** argv, Options & opts)
{
    for (int i = 1; i < argc; ++i)
    {
        const char * arg = argv[i];
        const bool hasValue = (i + 1 < argc);

        if (std::strcmp(arg, "--format") == 0 && hasValue)
        {
            const char * format = argv[++i];
            if (std::strcmp(format, "pal") == 0)
            {
                opts.format = Format::Pal;
            }
            else if (std::strcmp(format, "rgba32") == 0)
            {
                opts.format = Format::Rgba32;
            }
            else if (std::strcmp(format, "rgb16") == 0)
            {
                opts.format = Format::Rgb16;
            }
            else
            {
                return Fail("--format takes pal, rgba32 or rgb16, not '%s'", format);
            }
        }
        else if (std::strcmp(arg, "--palette") == 0 && hasValue)
        {
            opts.palettePath = argv[++i];
        }
        else if (std::strcmp(arg, "--out") == 0 && hasValue)
        {
            opts.outDir = argv[++i];
        }
        else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0)
        {
            return false;
        }
        else if (arg[0] == '-' && arg[1] != '\0')
        {
            return Fail("unknown or incomplete option '%s'", arg);
        }
        else
        {
            opts.images.push_back(arg);
        }
    }

    if (opts.images.empty() && opts.palettePath == nullptr)
    {
        return Fail("nothing to dump: give one or more pics, or --palette");
    }
    if (opts.format != Format::Pal && opts.palettePath == nullptr)
    {
        return Fail("--format rgba32 and rgb16 need the --palette to expand through");
    }
    return true;
}

std::string OutputPath(const Options & opts, const std::string & fileName)
{
    if (opts.outDir.empty() || opts.outDir.back() == '/')
    {
        return opts.outDir + fileName;
    }
    return opts.outDir + "/" + fileName;
}

} // namespace

int main(int argc, char ** argv)
{
    Options opts;
    if (!ParseArgs(argc, argv, opts))
    {
        PrintUsage();
        return 1;
    }

    // Two inputs with one name would write one file over the other.
    std::vector<std::string> names;
    for (const char * imagePath : opts.images)
    {
        const std::string name = SymbolName(imagePath);
        for (const std::string & seen : names)
        {
            if (seen == name)
            {
                Fail("two pics would both be dumped as '%s'", name.c_str());
                return 1;
            }
        }
        if (opts.palettePath != nullptr && name == "palette")
        {
            Fail("a pic named 'palette' would overwrite palette.cpp");
            return 1;
        }
        names.push_back(name);
    }

    Palette palette = {};
    if (opts.palettePath != nullptr)
    {
        const std::string outPath = OutputPath(opts, "palette.cpp");
        if (!LoadPalette(opts.palettePath, palette) || !WritePalette(outPath, palette))
        {
            return 1;
        }
        std::printf("%s -> %s (global_palette)\n", opts.palettePath, outPath.c_str());
    }

    for (size_t i = 0; i < opts.images.size(); ++i)
    {
        const char * imagePath = opts.images[i];
        Image image;
        if (!LoadPic(imagePath, image))
        {
            return 1;
        }

        std::vector<std::uint8_t> bytes;
        const char * contents = "";
        switch (opts.format)
        {
        case Format::Pal:
            bytes    = image.indices;
            contents = "8-bit palette indices into gfx/palette.lmp";
            break;
        case Format::Rgba32:
            bytes    = ExpandRgba32(image, palette);
            contents = "RGBA 8:8:8:8 through gfx/palette.lmp";
            break;
        case Format::Rgb16:
            bytes    = ExpandRgb16(image, palette);
            contents = "RGB 5:5:5:1 through gfx/palette.lmp";
            break;
        }

        const std::string outPath = OutputPath(opts, names[i] + ".cpp");
        if (!WriteImage(outPath, names[i], contents, image, bytes))
        {
            return 1;
        }
        std::printf("%s -> %s (%dx%d, %d bytes)\n", imagePath, outPath.c_str(), image.width, image.height,
                    static_cast<int>(bytes.size()));
    }
    return 0;
}

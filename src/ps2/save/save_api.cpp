/* ================================================================================================
 * File: save_api.cpp
 * Brief: The PS2_Save* and PS2_Config* hooks QuakeSpasm saves, loads and keeps its settings
 *        through (engine_hooks.h), and the choice of save device. See save_system.h for the
 *        overview.
 *
 *  Saves go to the memory card in MEMORY CARD slot 1. Running from the emulator's host:
 *  filesystem, the archived ps2_savedevice cvar can keep them as QuakeSpasm keeps them instead,
 *  plain <gamedir>/<name>.sav files ("host", the default there) - easy to look at, to back up,
 *  to swap with a desktop QuakeSpasm, and no formatted virtual card needed.
 *
 *  config.cfg is kept where the platform keeps settings: under the emulator, the host file
 *  QuakeSpasm always wrote, which also wins when reading - it is the one edited by hand while
 *  developing - with the card holding a copy when saves go there; on a console, the memory card,
 *  never the USB stick or hard disk the game data is on, whose own config.cfg only stands in
 *  when the card has none.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"
#include "ps2/save/slot_archive.h"
#include "ps2/save/memcard.h"
#include "ps2/engine_hooks.h"

#include <cstdlib>
#include <cstring>

namespace ps2::save {
namespace {

// The config's name in the card's save directory, and the most it is trusted to hold: the
// engine's own config.cfg is a few KB, so anything far bigger is not one it wrote.
constexpr const char * kConfigFile = "config.cfg";
constexpr u32 kMaxConfigBytes = 64u * 1024u;

// The config text being written: Host_WriteConfiguration's stream, over open_memstream.
static char * s_configText  = nullptr;
static size_t s_configBytes = 0;

// "host" or "mc". Only read when running from host: - on a console, saves always go to the card.
static cvar_t s_saveDevice = ps2::MakeCvar("ps2_savedevice", "host", CVAR_ARCHIVE);

// The save being written to the card: the stream Host_Savegame_f writes through, the slot it
// goes to, and the deflated text the stream fills.
static std::FILE * s_writeStream = nullptr;
static char s_writeSlot[kMaxSlotNameLen + 1] = {};
static Blob s_writeBlob;

// The card's state, probed at most once a frame: M_ScanSaves asks after twenty slots in a row,
// and a missing card would otherwise say so twenty times.
static int  s_probedFrame = -1;
static bool s_probeOk     = false;

bool ProbeCardOncePerFrame()
{
    if (host_framecount != s_probedFrame)
    {
        s_probedFrame = host_framecount;
        s_probeOk     = GetMemoryCardDevice().Probe();
    }
    return s_probeOk;
}

bool HostFilesAvailable()
{
    return std::strncmp(com_gamedir, "host:", 5) == 0;
}

bool SavesToCard()
{
    return !HostFilesAvailable() || q_strcasecmp(s_saveDevice.string, "host") != 0;
}

// The slot a save path names: QuakeSpasm passes <gamedir>/<name>.sav, and a slot is the name.
// False (SetError) if it can't be one: too long for a card file name, or with characters a card
// directory entry shouldn't hold.
bool SlotFromPath(const char * path, char (&outSlot)[kMaxSlotNameLen + 1])
{
    const char * const name = COM_SkipPath(path);
    const char * const dot  = std::strrchr(name, '.');
    const size_t length     = (dot != nullptr) ? static_cast<size_t>(dot - name) : std::strlen(name);

    if (length == 0 || length > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save names on the memory card can be 1 to %d characters long.", kMaxSlotNameLen);
        return false;
    }

    for (size_t i = 0; i < length; ++i)
    {
        const char c = name[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == '_' || c == '-';
        if (!allowed)
        {
            SetError("Save names on the memory card can only have letters, digits, '_' and '-'.");
            return false;
        }
    }

    std::memcpy(outSlot, name, length);
    outSlot[length] = '\0';
    return true;
}

// ------------------------------------------------------------------------------------------------
// config.cfg
// ------------------------------------------------------------------------------------------------

// <gamedir>/config.cfg - the emulator's host file, written the way QuakeSpasm always has.
void WriteHostConfig(const char * text, const u32 sizeBytes)
{
    char path[MAX_OSPATH];
    q_snprintf(path, sizeof(path), "%s/%s", com_gamedir, kConfigFile);

    std::FILE * const file = std::fopen(path, "w");
    bool written = (file != nullptr) && (std::fwrite(text, 1, sizeBytes, file) == sizeBytes);
    if (file != nullptr)
    {
        written = (std::fclose(file) == 0) && written;
    }

    if (!written)
    {
        Con_Printf("Couldn't write %s.\n", path);
    }
}

// The card's copy. Not worth bothering the player over - this runs on leaving the options menu
// and on quitting - so a card that isn't there or has no room just means the console says so.
void WriteCardConfig(const char * text, const u32 sizeBytes)
{
    Device & card = GetMemoryCardDevice();
    if (!card.Probe() || !card.EnsureSaveDir())
    {
        Con_Printf("config.cfg not saved to the memory card.\n");
        return;
    }

    if (FileMatches(card, kConfigFile, text, sizeBytes))
    {
        return; // The card has it already: no write.
    }

    u32 oldSizeBytes = 0;
    const u32 reclaimable = card.FileSize(kConfigFile, oldSizeBytes) ? card.FileCostBytes(oldSizeBytes) : 0u;
    if (card.FreeBytes() + reclaimable < card.FileCostBytes(sizeBytes))
    {
        Con_Printf("No room on the memory card for config.cfg.\n");
        return;
    }

    if (!WriteWholeFile(card, kConfigFile, text, sizeBytes))
    {
        card.Delete(kConfigFile); // Half a config would only be read back as one.
        Con_Printf("Couldn't save config.cfg to the memory card.\n");
        return;
    }
    Con_Printf("config.cfg saved to %s.\n", card.Describe(kConfigFile));
}

// The card's config.cfg in a block from `allocate`, with a 0 after it. Null if there is none.
template<typename AllocateFn>
char * ReadCardConfig(AllocateFn allocate)
{
    Device & card = GetMemoryCardDevice();
    u32 sizeBytes = 0;
    if (!card.Probe() || !card.FileSize(kConfigFile, sizeBytes) || sizeBytes == 0 || sizeBytes > kMaxConfigBytes)
    {
        return nullptr;
    }

    char * const text = allocate(sizeBytes + 1u);
    if (text == nullptr)
    {
        return nullptr;
    }

    const FileHandle handle = card.Open(kConfigFile, OpenMode::Read);
    const bool read = (handle != FileHandle::Invalid) && card.Read(handle, text, sizeBytes);
    if (handle != FileHandle::Invalid)
    {
        card.Close(handle);
    }

    if (!read)
    {
        Con_Printf("Couldn't read config.cfg from the memory card.\n");
        return nullptr; // The caller gives the block back.
    }

    text[sizeBytes] = '\0';
    Con_Printf("config.cfg read from %s.\n", card.Describe(kConfigFile));
    return text;
}

char * ReadCardConfigMalloc()
{
    char * block = nullptr;
    char * const text = ReadCardConfig([&block](const u32 sizeBytes) {
        block = static_cast<char *>(std::malloc(sizeBytes));
        return block;
    });
    if (text == nullptr)
    {
        std::free(block); // A failed read leaves the block behind.
    }
    return text;
}

// The game data's config.cfg - the host file, or one on a console's USB stick or hard disk -
// through the search path, as QuakeSpasm reads it, in a malloc block with a 0 after it.
char * ReadGameDataConfigMalloc()
{
    return static_cast<char *>(static_cast<void *>(COM_LoadMallocFile(kConfigFile, nullptr)));
}

// CFG_ReadCvars over config text: each line written as Cvar_WriteVariables writes it,
// <name> "<value>", sets the cvar it names if that is one of `vars`.
void SetCvarsFromConfig(char * text, const char ** vars, const int numVars)
{
    int found = 0;
    for (char * line = text; line != nullptr && *line != '\0' && found < numVars;)
    {
        char * const end = std::strchr(line, '\n');
        if (end != nullptr)
        {
            *end = '\0';
        }

        // As CFG_ReadCvars: tabs read as spaces, trailing spaces and the line end go, and the
        // line must end with the value's closing quotation mark.
        size_t length = std::strlen(line);
        for (size_t i = 0; i < length; ++i)
        {
            line[i] = (line[i] == '\t') ? ' ' : line[i];
        }
        while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\r'))
        {
            line[--length] = '\0';
        }

        if (length > 0 && line[length - 1] == '"')
        {
            line[length - 1] = '\0';
            for (int i = 0; i < numVars; ++i)
            {
                const size_t nameLength = std::strlen(vars[i]);
                const char * const quote = std::strchr(line, '"');
                if (std::strncmp(line, vars[i], nameLength) == 0 && line[nameLength] == ' ' && quote != nullptr)
                {
                    Cvar_Set(vars[i], quote + 1);
                    ++found;
                    break;
                }
            }
        }

        line = (end != nullptr) ? end + 1 : nullptr;
    }
}

void SaveInfoCommand()
{
    if (!SavesToCard())
    {
        Con_Printf("Saves go to host files: %s/<name>.sav (ps2_savedevice \"host\")\n", com_gamedir);
        return;
    }

    Device & card = GetMemoryCardDevice();
    const bool ready = card.Probe();
    Con_Printf("Save device: %s\n", card.StatusText());
    if (!ready)
    {
        return;
    }

    DirEntry files[64];
    const int count = card.List(files, ps2::ArrayLength(files));
    if (count < 0)
    {
        Con_Printf("  (can't list the save directory)\n");
    }
    for (int i = 0; i < count; ++i)
    {
        Con_Printf("  %-32s %7u bytes\n", files[i].name, static_cast<unsigned>(files[i].sizeBytes));
    }
}

} // namespace

void Init()
{
    Cvar_RegisterVariable(&s_saveDevice);
    Cmd_AddCommand("ps2_saveinfo", &SaveInfoCommand);
}

void ReadConfigCvars(const char ** vars, const int numVars)
{
    // Under the emulator the host file comes first: it is the one edited by hand while
    // developing. On a console the card's does: it is the player's own, and the game data's only
    // stands in for one the card doesn't have. Whatever ps2_savedevice says: this runs before
    // config.cfg has set it.
    char * text = nullptr;
    if (HostFilesAvailable())
    {
        text = ReadGameDataConfigMalloc();
        text = (text != nullptr) ? text : ReadCardConfigMalloc();
    }
    else
    {
        text = ReadCardConfigMalloc();
        text = (text != nullptr) ? text : ReadGameDataConfigMalloc();
    }

    if (text != nullptr)
    {
        SetCvarsFromConfig(text, vars, numVars);
        std::free(text);
    }
}

} // namespace ps2::save

// ------------------------------------------------------------------------------------------------
// PS2_Save* - the engine's hooks (engine_hooks.h)
// ------------------------------------------------------------------------------------------------

using namespace ps2::save;

extern "C" {

FILE * PS2_SaveOpenWrite(const char * path)
{
    ClearError();

    if (!SavesToCard())
    {
        Con_Printf("Saving game to %s...\n", path);
        return std::fopen(path, "w");
    }

    // A Host_Error in the middle of the last save unwound past its close: drop what it wrote.
    if (s_writeStream != nullptr)
    {
        ClosePackStream(s_writeStream);
        BlobFree(s_writeBlob);
        s_writeStream = nullptr;
    }

    if (!SlotFromPath(path, s_writeSlot))
    {
        return nullptr;
    }

    Con_Printf("Saving game to the memory card, slot '%s'...\n", s_writeSlot);
    s_writeStream = OpenPackStream(s_writeBlob);
    return s_writeStream;
}

qboolean PS2_SaveCloseWrite(FILE * f, const char * comment)
{
    if (f != s_writeStream)
    {
        return (std::fclose(f) == 0) ? 1 : 0; // A host file.
    }

    s_writeStream = nullptr;
    const bool stored = ClosePackStream(f) && StoreSlot(GetMemoryCardDevice(), s_writeSlot, comment, s_writeBlob);
    BlobFree(s_writeBlob);
    return stored ? 1 : 0;
}

char * PS2_SaveLoadText(const char * path)
{
    ClearError();

    if (!SavesToCard())
    {
        Con_Printf("Loading game from %s...\n", path);
        return static_cast<char *>(static_cast<void *>(COM_LoadMallocFile_TextMode_OSPath(path, nullptr)));
    }

    char slot[kMaxSlotNameLen + 1];
    if (!SlotFromPath(path, slot))
    {
        return nullptr;
    }
    Con_Printf("Loading game from the memory card, slot '%s'...\n", slot);

    Blob save;
    if (!RestoreSlot(GetMemoryCardDevice(), slot, save))
    {
        return nullptr;
    }

    char * const text = InflateBlobToText(save);
    BlobFree(save);
    return text;
}

qboolean PS2_SaveReadComment(const char * path, char * comment, const int size)
{
    if (size <= 0)
    {
        return 0;
    }
    comment[0] = '\0';

    if (!SavesToCard())
    {
        // M_ScanSaves's own reading: the version line, then the comment.
        std::FILE * const f = std::fopen(path, "r");
        if (f == nullptr)
        {
            return 0;
        }

        int version = 0;
        char line[MAX_OSPATH];
        const bool read = std::fscanf(f, "%i\n", &version) == 1 && std::fscanf(f, "%79s\n", line) == 1;
        std::fclose(f);
        if (read)
        {
            q_strlcpy(comment, line, static_cast<size_t>(size));
        }
        return read ? 1 : 0;
    }

    char slot[kMaxSlotNameLen + 1];
    if (!SlotFromPath(path, slot) || !ProbeCardOncePerFrame())
    {
        return 0;
    }

    const SlotInfo info = ReadSlotInfo(GetMemoryCardDevice(), slot);
    if (info.state != SlotState::Valid)
    {
        return 0;
    }
    q_strlcpy(comment, info.comment, static_cast<size_t>(size));
    return 1;
}

// ------------------------------------------------------------------------------------------------
// PS2_Config* - config.cfg (engine_hooks.h)
// ------------------------------------------------------------------------------------------------

FILE * PS2_ConfigOpenWrite(void)
{
    std::free(s_configText); // From a write a Host_Error cut short.
    s_configText  = nullptr;
    s_configBytes = 0;

    std::FILE * const f = open_memstream(&s_configText, &s_configBytes);
    if (f == nullptr)
    {
        Con_Printf("Couldn't write config.cfg: out of memory.\n");
    }
    return f;
}

void PS2_ConfigCloseWrite(FILE * f)
{
    const bool built = (std::fclose(f) == 0) && s_configText != nullptr;
    if (built && s_configBytes != 0)
    {
        const u32 sizeBytes = static_cast<u32>(s_configBytes);

        // The emulator's host file, as QuakeSpasm always wrote it. Never a console's USB stick
        // or hard disk: on a console the memory card is where settings are kept.
        if (HostFilesAvailable())
        {
            WriteHostConfig(s_configText, sizeBytes);
        }

        // The card, whenever saves go there: always on a console, and under the emulator when
        // ps2_savedevice says "mc".
        if (SavesToCard())
        {
            WriteCardConfig(s_configText, sizeBytes);
        }
    }

    std::free(s_configText);
    s_configText  = nullptr;
    s_configBytes = 0;
}

const char * PS2_ConfigLoadHunk(void)
{
    // A card read that fails gives its hunk block back: Cmd_Exec_f only frees to its low mark
    // when it gets a config to run.
    const auto fromCard = []() -> const char * {
        const int mark = Hunk_LowMark();
        const char * const text = ReadCardConfig([](const u32 sizeBytes) {
            return static_cast<char *>(Hunk_AllocName(static_cast<int>(sizeBytes), "config.cfg"));
        });
        if (text == nullptr)
        {
            Hunk_FreeToLowMark(mark);
        }
        return text;
    };
    const auto fromGameData = []() {
        return static_cast<const char *>(static_cast<const void *>(COM_LoadHunkFile(kConfigFile, nullptr)));
    };

    // As ReadConfigCvars: the host file first under the emulator, the card first on a console.
    if (HostFilesAvailable())
    {
        const char * const text = fromGameData();
        return (text != nullptr) ? text : fromCard();
    }

    const char * const text = fromCard();
    return (text != nullptr) ? text : fromGameData();
}

} // extern "C"

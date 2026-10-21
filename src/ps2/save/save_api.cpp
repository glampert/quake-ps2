/* ================================================================================================
 * File: save_api.cpp
 * Brief: The PS2_Save* and PS2_Config* hooks QuakeSpasm saves, loads and keeps its settings
 *        through (engine_hooks.h), and the choice of save device. See save_system.h for the
 *        overview.
 *
 *  Saves go to the memory card in MEMORY CARD slot 1, or, when the archived ps2_savedevice cvar
 *  says so, to the drive the game booted from: "host" for the emulator's host: filesystem, "usb"
 *  for a USB drive, "hdd" for the hard disk. A value naming a drive the game didn't boot from
 *  means the card, so a config.cfg carried from one to another still saves somewhere. Every
 *  device holds the same slot archives (slot_archive.cpp), under the same save notice
 *  (save_notice.h).
 *
 *  config.cfg lives with the saves. On a console, the boot drive's own id1/config.cfg is taken
 *  when it says saves go to that drive: the player keeps everything there, and the card isn't
 *  touched. Otherwise the card's is, and the drive's only stands in for one the card hasn't got.
 *  It is written to the save device, and a switch from the drive back to the card rewrites the
 *  drive's once more, so that it no longer names itself. Under the emulator the host file comes
 *  first and is always written, whatever the save device: it is the tracked id1/config.cfg every
 *  development run starts from, and the card holds a copy when saves go there.
 *
 *  A save or a load that fails says why in a message box the player dismisses, with the way to
 *  the other device; the save menus say so before a slot is picked when the device can't be used.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"
#include "ps2/save/slot_archive.h"
#include "ps2/save/memcard.h"
#include "ps2/save/drive.h"
#include "ps2/save/save_notice.h"
#include "ps2/engine_hooks.h"
#include "ps2/system/iop_boot.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2::save {
namespace {

// The config's name, in the card's save directory and in the game directory, and the most it is
// trusted to hold: the engine's own config.cfg is a few KB, so anything far bigger is not one it
// wrote.
constexpr const char * kConfigFile = "config.cfg";
constexpr u32 kMaxConfigBytes = 64u * 1024u;

// The config text being written: Host_WriteConfiguration's stream, over open_memstream.
static char * s_configText  = nullptr;
static size_t s_configBytes = 0;

// "mc", or the boot drive's "host", "usb" or "hdd" (DriveValue).
static cvar_t s_saveDevice = ps2::MakeCvar("ps2_savedevice", "mc", CVAR_ARCHIVE);

// The save being written: the stream Host_Savegame_f writes through, the device and slot it goes
// to, and the deflated text the stream fills.
static std::FILE * s_writeStream = nullptr;
static Device *    s_writeDevice = nullptr;
static char        s_writeSlot[kMaxSlotNameLen + 1] = {};
static Blob        s_writeBlob;

// The save device's state, probed at most once a frame: M_ScanSaves asks after twenty slots in a
// row, and a missing card would otherwise say so twenty times.
static int      s_probedFrame  = -1;
static Device * s_probedDevice = nullptr;
static bool     s_probeOk      = false;

// M_ScanSaves's save notice is up, put up by PS2_SaveScanBegin for its twenty slots.
static bool s_scanNotice = false;

// Why the save menus' last look at the save device (PS2_SaveScanBegin) found it unusable: empty
// when it could be used.
static char s_menuProblem[160] = {};

// The cvar's value for a drive.
const char * DriveValue(const DeviceKind kind)
{
    switch (kind)
    {
    case DeviceKind::HostFiles:  return "host";
    case DeviceKind::UsbDrive:   return "usb";
    case DeviceKind::HardDisk:   return "hdd";
    case DeviceKind::MemoryCard: break;
    }
    return "mc";
}

bool HostFilesAvailable()
{
    return std::strncmp(com_gamedir, "host:", 5) == 0;
}

Device & ActiveDevice()
{
    Device & drive = GetDriveDevice();
    return (q_strcasecmp(s_saveDevice.string, DriveValue(drive.Kind())) == 0) ? drive : GetMemoryCardDevice();
}

bool SavesToCard()
{
    return &ActiveDevice() == &GetMemoryCardDevice();
}

// The device the options menu's Save Device row switches to from `device`.
Device & OtherDevice(const Device & device)
{
    return (&device == &GetMemoryCardDevice()) ? GetDriveDevice() : GetMemoryCardDevice();
}

bool ProbeOncePerFrame(Device & device)
{
    if (host_framecount != s_probedFrame || s_probedDevice != &device)
    {
        s_probedFrame  = host_framecount;
        s_probedDevice = &device;
        s_probeOk      = device.Probe();
    }
    return s_probeOk;
}

// The slot a save path names: QuakeSpasm passes <gamedir>/<name>.sav, and a slot is the name.
// False (SetError) if it can't be one: too long for a card file name, or with characters a card
// directory entry shouldn't hold. The same names go on every device.
bool SlotFromPath(const char * path, char (&outSlot)[kMaxSlotNameLen + 1])
{
    const char * const name = COM_SkipPath(path);
    const char * const dot  = std::strrchr(name, '.');
    const size_t length     = (dot != nullptr) ? static_cast<size_t>(dot - name) : std::strlen(name);

    if (length == 0 || length > static_cast<size_t>(kMaxSlotNameLen))
    {
        SetError("Save names can be 1 to %d characters long.", kMaxSlotNameLen);
        return false;
    }

    for (size_t i = 0; i < length; ++i)
    {
        const char c = name[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == '_' || c == '-';
        if (!allowed)
        {
            SetError("Save names can only have letters, digits, '_' and '-'.");
            return false;
        }
    }

    std::memcpy(outSlot, name, length);
    outSlot[length] = '\0';
    return true;
}

// ------------------------------------------------------------------------------------------------
// Telling the player
// ------------------------------------------------------------------------------------------------

// The message boxes' lines are at most this long, broken between words: the menu canvas is 320
// units, 40 characters, wide, and a box's border takes one on each side.
constexpr int kMaxLineChars = 38;

// Appends `text` to `out`, a string in a buffer of `outSize`, as lines of at most kMaxLineChars
// characters, each ended by a '\n'.
void AppendWrapped(char * out, const size_t outSize, const char * text)
{
    for (const char * line = text; *line != '\0';)
    {
        while (*line == ' ')
        {
            ++line;
        }

        // The most whole words that fit; a word too long for a line on its own is cut.
        int length = 0;
        int fits   = 0;
        while (line[length] != '\0' && length < kMaxLineChars)
        {
            ++length;
            if (line[length] == '\0' || line[length] == ' ')
            {
                fits = length;
            }
        }
        fits = (fits != 0) ? fits : length;

        const size_t used = std::strlen(out);
        if (fits != 0 && used < outSize)
        {
            std::snprintf(out + used, outSize - used, "%.*s\n", fits, line);
        }
        line += fits;
    }
}

// What went wrong, as the device said; a device that refused without saying why is the rare
// case of a notice that couldn't be drawn.
const char * Reason()
{
    return (LastError()[0] != '\0') ? LastError() : "The save device can't be used right now.";
}

// How to carry on with the other device.
void AppendAdvice(char * out, const size_t outSize, const Device & device)
{
    char advice[96];
    std::snprintf(advice, sizeof(advice), "To use the %s instead, change Save Device in the Options menu.",
                  DeviceName(OtherDevice(device).Kind()));
    AppendWrapped(out, outSize, advice);
}

// A failed save or load, in a message box the player dismisses: what failed, why, and how to use
// the other device. QuakeSpasm says it only on the console, whose lines go by over the game in a
// few seconds, too small to read on a TV. SCR_ModalMessage draws it and waits for a button.
void ReportFailure(const Device & device, const SaveAccess access)
{
    char text[512] = {};
    std::snprintf(text, sizeof(text), "%s\n\n", (access == SaveAccess::Saving) ? "Couldn't save the game." : "Couldn't load the game.");
    AppendWrapped(text, sizeof(text), Reason());
    q_strlcat(text, "\n", sizeof(text));
    AppendAdvice(text, sizeof(text), device);
    q_strlcat(text, "\nPress Cross to continue.\n", sizeof(text));
    SCR_ModalMessage(text, 0.0f);
}

// ------------------------------------------------------------------------------------------------
// Loading
// ------------------------------------------------------------------------------------------------

// PS2_SaveLoadText's reading: the slot read under the save notice - up already when the load
// menu put it up (PS2_SaveLoadStarting) - and inflated back to text.
char * LoadSlotText(Device & device, const char * path)
{
    char slot[kMaxSlotNameLen + 1];
    Blob save;
    bool restored = false;
    if (SlotFromPath(path, slot))
    {
        Con_Printf("Loading game from the %s, slot '%s'...\n", DeviceName(device.Kind()), slot);
        restored = BeginSaveNotice(device, SaveAccess::Loading) && RestoreSlot(device, slot, save);
    }
    EndSaveNotice();

    if (!restored)
    {
        return nullptr;
    }

    char * const text = InflateBlobToText(save);
    BlobFree(save);
    return text;
}

// A load that fails leaves the load menu's loading plaque up - QuakeSpasm's Host_Loadgame_f
// returns without taking it down - until its timeout, over a game still running. On a console
// that only takes a card pulled since the menu listed the slots. This is what
// SCR_EndLoadingPlaque does, but without its Con_ClearNotify, so the lines saying why stay on
// the screen.
void EndPlaqueOfFailedLoad()
{
    if (scr_disabled_for_loading)
    {
        scr_disabled_for_loading = false;
        PS2_LoadingScreenEnd();
    }
}

// ------------------------------------------------------------------------------------------------
// config.cfg in general
// ------------------------------------------------------------------------------------------------

// `sizeBytes` of text in a malloc block, with a 0 after it; null if out of memory.
char * DuplicateText(const char * text, const u32 sizeBytes)
{
    char * const copy = static_cast<char *>(std::malloc(sizeBytes + 1u));
    if (copy != nullptr)
    {
        std::memcpy(copy, text, sizeBytes);
        copy[sizeBytes] = '\0';
    }
    return copy;
}

// The value config text gives a cvar, from the last line naming it, as running it would leave
// it, in `out`; false if no line does. Lines as Cvar_WriteVariables writes them: <name> "<value>".
bool ConfigValue(const char * text, const char * name, char * out, const size_t outSize)
{
    const size_t nameLength = std::strlen(name);
    bool found = false;
    for (const char * line = text; *line != '\0';)
    {
        const char * const end = std::strchr(line, '\n');
        const size_t length = (end != nullptr) ? static_cast<size_t>(end - line) : std::strlen(line);

        if (length > nameLength + 2 && std::strncmp(line, name, nameLength) == 0 && line[nameLength] == ' ' &&
            line[nameLength + 1] == '"')
        {
            const char * const value = line + nameLength + 2;
            const char * const quote = static_cast<const char *>(std::memchr(value, '"', length - nameLength - 2));
            if (quote != nullptr)
            {
                std::snprintf(out, outSize, "%.*s", static_cast<int>(quote - value), value);
                found = true;
            }
        }
        line += length + ((end != nullptr) ? 1 : 0);
    }
    return found;
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

// ------------------------------------------------------------------------------------------------
// config.cfg on the memory card
// ------------------------------------------------------------------------------------------------

// What the card's config.cfg holds, as far as this run knows. The card is read for it once, by
// the boot's first read that wants it, and the copy follows every write. The boot's later reads
// (IN_Init's, then quake.rc's "exec config.cfg"), any later "exec config.cfg", and a config
// written unchanged, as on leaving the options menu with nothing changed, are all answered from
// it: each would otherwise go to the card under the save notice for nothing. A probe that finds
// another card in the slot, or none, makes it stale (Device::Generation).
enum class CardConfig : u8
{
    Unknown, // Not read or written yet, or written and failed.
    Absent,  // The card has none, or none this game would have written (empty, or far too big).
    Present, // The card holds s_cardConfigText.
};

static CardConfig s_cardConfig           = CardConfig::Unknown;
static char *     s_cardConfigText       = nullptr; // malloc: s_cardConfigBytes, then a 0
static u32        s_cardConfigBytes      = 0;
static u32        s_cardConfigGeneration = 0;

// Whether the copy describes the card in the slot, as of the last probe.
bool CardConfigCurrent()
{
    return s_cardConfig != CardConfig::Unknown && s_cardConfigGeneration == GetMemoryCardDevice().Generation();
}

// Records what the card holds now: `text` (taken over: a block from DuplicateText), or nothing
// for Absent and Unknown. A text that couldn't be copied leaves it unknown.
void KeepCardConfig(const CardConfig state, char * const text, const u32 sizeBytes)
{
    std::free(s_cardConfigText);
    s_cardConfig           = (state == CardConfig::Present && text == nullptr) ? CardConfig::Unknown : state;
    s_cardConfigText       = text;
    s_cardConfigBytes      = (text != nullptr) ? sizeBytes : 0u;
    s_cardConfigGeneration = GetMemoryCardDevice().Generation();
}

// Reads the card's config.cfg into the copy, under the save notice: the directory too, to find
// whether there is one, as every read of the card's data goes under it. A card that isn't there,
// or can't be read, leaves the copy unknown.
void ReadCardConfigIntoCopy()
{
    Device & card = GetMemoryCardDevice();
    if (!BeginSaveNotice(card, SaveAccess::Loading))
    {
        return;
    }

    u32 sizeBytes = 0;
    if (!card.FileSize(kConfigFile, sizeBytes) || sizeBytes == 0 || sizeBytes > kMaxConfigBytes)
    {
        EndSaveNotice();
        KeepCardConfig(CardConfig::Absent, nullptr, 0);
        return;
    }

    char * const text = static_cast<char *>(std::malloc(sizeBytes + 1u));
    const FileHandle handle = (text != nullptr) ? card.Open(kConfigFile, OpenMode::Read) : FileHandle::Invalid;
    const bool read = (handle != FileHandle::Invalid) && card.Read(handle, text, sizeBytes);
    if (handle != FileHandle::Invalid)
    {
        card.Close(handle);
    }
    EndSaveNotice();

    if (!read)
    {
        std::free(text);
        Con_Printf("Couldn't read config.cfg from the memory card.\n");
        return;
    }

    text[sizeBytes] = '\0';
    KeepCardConfig(CardConfig::Present, text, sizeBytes);
    Con_Printf("config.cfg read from %s.\n", card.Describe(kConfigFile));
}

// The card's config.cfg, in a malloc block of the caller's, which may write into it; null if it
// has none or can't be read.
char * CardConfigMalloc()
{
    if (!CardConfigCurrent())
    {
        ReadCardConfigIntoCopy();
    }
    return (s_cardConfig == CardConfig::Present) ? DuplicateText(s_cardConfigText, s_cardConfigBytes) : nullptr;
}

// WriteCardConfig's write, under the save notice.
void StoreCardConfig(const char * text, const u32 sizeBytes)
{
    Device & card = GetMemoryCardDevice();
    if (!card.EnsureSaveDir())
    {
        Con_Printf("config.cfg not saved to the memory card.\n");
        return;
    }

    // With the copy stale (another card) or never taken (the emulator reads the host file), the
    // card may hold these very bytes: no write then.
    if (!CardConfigCurrent() && FileMatches(card, kConfigFile, text, sizeBytes))
    {
        KeepCardConfig(CardConfig::Present, DuplicateText(text, sizeBytes), sizeBytes);
        return;
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
        KeepCardConfig(CardConfig::Unknown, nullptr, 0);
        Con_Printf("Couldn't save config.cfg to the memory card.\n");
        return;
    }
    KeepCardConfig(CardConfig::Present, DuplicateText(text, sizeBytes), sizeBytes);
    Con_Printf("config.cfg saved to %s.\n", card.Describe(kConfigFile));
}

// The card's copy. Not worth bothering the player over - this runs on leaving the options menu
// and on quitting - so a card that isn't there or has no room just means the console says so. A
// config the card holds already isn't written, nor even looked for: the card is only touched,
// under the save notice, for one it doesn't have.
void WriteCardConfig(const char * text, const u32 sizeBytes)
{
    if (CardConfigCurrent() && s_cardConfig == CardConfig::Present && s_cardConfigBytes == sizeBytes &&
        std::memcmp(s_cardConfigText, text, sizeBytes) == 0)
    {
        return;
    }

    if (!BeginSaveNotice(GetMemoryCardDevice(), SaveAccess::Saving))
    {
        Con_Printf("config.cfg not saved to the memory card.\n");
        return;
    }
    StoreCardConfig(text, sizeBytes);
    EndSaveNotice();
}

// ------------------------------------------------------------------------------------------------
// config.cfg on the boot drive
// ------------------------------------------------------------------------------------------------

// What the drive's config.cfg - the game data's, <gamedir>/config.cfg, the one QuakeSpasm keeps -
// holds as far as this run knows: read by each read of it, and following every write, which the
// next write compares against. Null for none.
static bool   s_driveConfigRead  = false;
static char * s_driveConfigText  = nullptr; // malloc: s_driveConfigBytes, then a 0
static u32    s_driveConfigBytes = 0;

void KeepDriveConfig(char * const text, const u32 sizeBytes)
{
    std::free(s_driveConfigText);
    s_driveConfigRead  = true;
    s_driveConfigText  = text;
    s_driveConfigBytes = (text != nullptr) ? sizeBytes : 0u;
}

// The drive's config.cfg through the search path, as QuakeSpasm reads it, in a malloc block of
// the caller's, or null; the copy follows it.
char * DriveConfigMalloc()
{
    char * const text = static_cast<char *>(static_cast<void *>(COM_LoadMallocFile(kConfigFile, nullptr)));
    const u32 sizeBytes = (text != nullptr) ? static_cast<u32>(com_filesize) : 0u;
    KeepDriveConfig((text != nullptr) ? DuplicateText(text, sizeBytes) : nullptr, sizeBytes);
    return text;
}

// Whether the drive's config.cfg says saves go to the drive: then it is the player's own, kept
// there, and not the game data's stand-in for a card without one.
bool DriveConfigNamesDrive()
{
    if (!s_driveConfigRead)
    {
        std::free(DriveConfigMalloc());
    }

    char value[16];
    return s_driveConfigText != nullptr && ConfigValue(s_driveConfigText, s_saveDevice.name, value, sizeof(value)) &&
           q_strcasecmp(value, DriveValue(GetDriveDevice().Kind())) == 0;
}

// Writes <gamedir>/config.cfg, unless it holds these bytes already; under the save notice when it
// is the save device's copy (`withNotice`).
void WriteDriveConfig(const char * text, const u32 sizeBytes, const bool withNotice)
{
    if (!s_driveConfigRead)
    {
        std::free(DriveConfigMalloc());
    }
    if (s_driveConfigText != nullptr && s_driveConfigBytes == sizeBytes && std::memcmp(s_driveConfigText, text, sizeBytes) == 0)
    {
        return;
    }

    Device & drive = GetDriveDevice();
    if (withNotice && !BeginSaveNotice(drive, SaveAccess::Saving))
    {
        Con_Printf("config.cfg not saved to the %s.\n", DeviceName(drive.Kind()));
        return;
    }

    char path[MAX_OSPATH];
    q_snprintf(path, sizeof(path), "%s/%s", com_gamedir, kConfigFile);

    std::FILE * const file = std::fopen(path, "w");
    bool written = (file != nullptr) && (std::fwrite(text, 1, sizeBytes, file) == sizeBytes);
    if (file != nullptr)
    {
        written = (std::fclose(file) == 0) && written;
    }
    written = (ps2::sys::SyncGameDataDevice() == 0) && written; // The hard disk's PFS keeps it cached otherwise.

    if (withNotice)
    {
        EndSaveNotice();
    }

    if (!written)
    {
        KeepDriveConfig(nullptr, 0); // Whatever is there now, it isn't these bytes for sure.
        s_driveConfigRead = false;
        Con_Printf("Couldn't write %s.\n", path);
        return;
    }
    KeepDriveConfig(DuplicateText(text, sizeBytes), sizeBytes);
}

// Where a console's boot takes config.cfg from, decided at its first read so that the three
// agree: VID_Init's and IN_Init's, for the cvars read ahead, and quake.rc's "exec config.cfg".
enum class ConfigSource : u8
{
    Undecided,
    Drive, // The drive's own names the drive as the save device.
    Card,  // The card's, and the drive's only if the card has none.
};

static ConfigSource s_configSource = ConfigSource::Undecided;

bool ConfigFromDrive()
{
    if (s_configSource == ConfigSource::Undecided)
    {
        s_configSource = DriveConfigNamesDrive() ? ConfigSource::Drive : ConfigSource::Card;
    }
    return s_configSource == ConfigSource::Drive;
}

// config.cfg's text, from where this run takes it, in a malloc block of the caller's; null if
// there is none anywhere. Whatever ps2_savedevice says now: the boot's reads come before
// config.cfg has set it.
char * ReadConfigMalloc()
{
    // Under the emulator the host file comes first: it is the one edited by hand while developing.
    if (HostFilesAvailable())
    {
        char * const text = DriveConfigMalloc();
        return (text != nullptr) ? text : CardConfigMalloc();
    }

    if (ConfigFromDrive())
    {
        return DriveConfigMalloc();
    }
    char * const text = CardConfigMalloc();
    return (text != nullptr) ? text : DriveConfigMalloc();
}

// ------------------------------------------------------------------------------------------------
// ps2_saveinfo
// ------------------------------------------------------------------------------------------------

void SaveInfoCommand()
{
    Device & device = ActiveDevice();
    Device & other  = OtherDevice(device);
    Con_Printf("Save devices here: %s (ps2_savedevice \"%s\"), %s (\"%s\").\n", DeviceName(device.Kind()),
               DriveValue(device.Kind()), DeviceName(other.Kind()), DriveValue(other.Kind()));

    // The listing is a read of the device, under the save notice like any other.
    const bool ready = BeginSaveNotice(device, SaveAccess::Loading);
    Con_Printf("Save device: %s\n", device.StatusText());
    if (!ready)
    {
        return;
    }

    DirEntry files[64];
    const int count = device.List(files, ps2::ArrayLength(files));
    EndSaveNotice();

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
    char * const text = ReadConfigMalloc();
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

    // A Host_Error in the middle of the last save unwound past its close: drop what it wrote.
    if (s_writeStream != nullptr)
    {
        ClosePackStream(s_writeStream);
        BlobFree(s_writeBlob);
        s_writeStream = nullptr;
        EndSaveNotice();
    }

    Device & device = ActiveDevice();
    if (!SlotFromPath(path, s_writeSlot))
    {
        ReportFailure(device, SaveAccess::Saving);
        return nullptr;
    }

    // The save notice goes up, and the music fades out, before the engine writes the game out:
    // that alone holds the mixer up for a few hundred milliseconds, and the card for longer.
    Con_Printf("Saving game to the %s, slot '%s'...\n", DeviceName(device.Kind()), s_writeSlot);
    if (!BeginSaveNotice(device, SaveAccess::Saving))
    {
        ReportFailure(device, SaveAccess::Saving);
        return nullptr;
    }

    s_writeStream = OpenPackStream(s_writeBlob);
    if (s_writeStream == nullptr)
    {
        EndSaveNotice(); // No close is coming to take it down.
        ReportFailure(device, SaveAccess::Saving);
        return nullptr;
    }
    s_writeDevice = &device;
    return s_writeStream;
}

qboolean PS2_SaveCloseWrite(FILE * f, const char * comment)
{
    if (f != s_writeStream || s_writeDevice == nullptr)
    {
        return (std::fclose(f) == 0) ? 1 : 0; // Not a save's stream: nothing of ours.
    }

    Device & device = *s_writeDevice;
    s_writeStream = nullptr;
    s_writeDevice = nullptr;

    const bool stored = ClosePackStream(f) && StoreSlot(device, s_writeSlot, comment, s_writeBlob);
    BlobFree(s_writeBlob);
    EndSaveNotice();
    if (!stored)
    {
        ReportFailure(device, SaveAccess::Saving);
    }
    return stored ? 1 : 0;
}

char * PS2_SaveLoadText(const char * path)
{
    ClearError();

    Device & device = ActiveDevice();
    char * const text = LoadSlotText(device, path);
    if (text == nullptr)
    {
        // The plaque first: the message box draws over the game, which it would hide.
        EndPlaqueOfFailedLoad();
        ReportFailure(device, SaveAccess::Loading);
    }
    return text;
}

void PS2_SaveLoadStarting(void)
{
    ClearError();
    BeginSaveNotice(ActiveDevice(), SaveAccess::Loading); // A device that can't be read fails the load itself.
}

qboolean PS2_SaveReadComment(const char * path, char * comment, const int size)
{
    if (size <= 0)
    {
        return 0;
    }
    comment[0] = '\0';

    Device & device = ActiveDevice();
    char slot[kMaxSlotNameLen + 1];
    if (!SlotFromPath(path, slot) || !ProbeOncePerFrame(device))
    {
        return 0;
    }

    // Under the save notice: M_ScanSaves's, which covers the twenty slots, or one of its own.
    const bool ownNotice = !SaveNoticeShown();
    if (ownNotice && !BeginSaveNotice(device, SaveAccess::Loading))
    {
        return 0;
    }
    const SlotInfo info = ReadSlotInfo(device, slot);
    if (ownNotice)
    {
        EndSaveNotice();
    }

    if (info.state != SlotState::Valid)
    {
        return 0;
    }
    q_strlcpy(comment, info.comment, static_cast<size_t>(size));
    return 1;
}

void PS2_SaveScanBegin(void)
{
    // Looked at afresh, as the menus open: a card put in since the last look counts. The slots'
    // reads then share this probe (ProbeOncePerFrame), so a device that can't be used says so
    // once, and gets no notice; nor does one under a notice already.
    Device & device = ActiveDevice();
    ClearError();
    s_probedFrame  = host_framecount;
    s_probedDevice = &device;
    s_probeOk      = device.Probe();

    CopyName(s_menuProblem, s_probeOk ? "" : Reason());
    s_scanNotice = s_probeOk && !SaveNoticeShown() && BeginSaveNotice(device, SaveAccess::Loading);
}

void PS2_SaveScanEnd(void)
{
    if (s_scanNotice)
    {
        s_scanNotice = false;
        EndSaveNotice();
    }
}

qboolean PS2_SaveMenuProblem(void)
{
    return (s_menuProblem[0] != '\0') ? 1 : 0;
}

void PS2_SaveMenuDrawProblem(void)
{
    if (s_menuProblem[0] == '\0')
    {
        return;
    }

    // Why, in white, as the save notice's warning is, then how to carry on, in gold.
    char reason[256] = {};
    char advice[256] = {};
    AppendWrapped(reason, sizeof(reason), s_menuProblem);
    AppendAdvice(advice, sizeof(advice), ActiveDevice());

    const char * texts[2] = { reason, advice };
    int numLines = 1; // The gap between the two.
    int longest  = 0;
    for (const char * text : texts)
    {
        for (const char * line = text; *line != '\0'; ++numLines)
        {
            const char * const end = std::strchr(line, '\n'); // AppendWrapped ends every line with one
            if (end == nullptr)
            {
                break;
            }
            longest = std::max(longest, static_cast<int>(end - line));
            line = end + 1;
        }
    }

    // As wide as the longest line with a space either side, at most the canvas, and even:
    // M_DrawTextBox draws its middle two characters at a time.
    constexpr int kGlyph = 8;
    const int chars = std::min((longest + 3) & ~1, kMaxLineChars);
    const int top   = (200 - ((numLines + 2) * kGlyph)) / 2;
    M_DrawTextBox((320 - ((chars + 2) * kGlyph)) / 2, top, chars, numLines);

    int y = top + kGlyph;
    for (int i = 0; i < 2; ++i)
    {
        for (const char * line = texts[i]; *line != '\0'; y += kGlyph)
        {
            const char * const end = std::strchr(line, '\n');
            if (end == nullptr)
            {
                break;
            }
            char text[kMaxLineChars + 1];
            q_strlcpy(text, line, std::min(static_cast<size_t>(end - line) + 1u, sizeof(text)));

            const int x = (320 - (static_cast<int>(std::strlen(text)) * kGlyph)) / 2;
            if (i == 0)
            {
                M_PrintWhite(x, y, text);
            }
            else
            {
                M_Print(x, y, text);
            }
            line = end + 1;
        }
        y += kGlyph; // The gap.
    }
}

// ------------------------------------------------------------------------------------------------
// PS2_Config* - config.cfg (engine_hooks.h)
// ------------------------------------------------------------------------------------------------

const char * PS2_SaveDeviceName(void)
{
    return DeviceName(ActiveDevice().Kind());
}

void PS2_SaveDeviceToggle(void)
{
    Cvar_Set(s_saveDevice.name, SavesToCard() ? DriveValue(GetDriveDevice().Kind()) : "mc");
}

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

        if (HostFilesAvailable())
        {
            // The emulator's host file, always - under the save notice when host files are where
            // saves go - and the card's copy when the card is.
            WriteDriveConfig(s_configText, sizeBytes, /*withNotice=*/!SavesToCard());
            if (SavesToCard())
            {
                WriteCardConfig(s_configText, sizeBytes);
            }
        }
        else if (!SavesToCard())
        {
            WriteDriveConfig(s_configText, sizeBytes, /*withNotice=*/true);
        }
        else
        {
            WriteCardConfig(s_configText, sizeBytes);

            // A drive config.cfg still naming the drive would be taken over the card's at the
            // next boot: rewritten once, it names the card.
            if (DriveConfigNamesDrive())
            {
                WriteDriveConfig(s_configText, sizeBytes, /*withNotice=*/true);
            }
        }
    }

    std::free(s_configText);
    s_configText  = nullptr;
    s_configBytes = 0;
}

const char * PS2_ConfigLoadHunk(void)
{
    char * const text = ReadConfigMalloc();
    if (text == nullptr)
    {
        return nullptr;
    }

    const size_t sizeBytes = std::strlen(text);
    char * const block = static_cast<char *>(Hunk_AllocName(static_cast<int>(sizeBytes + 1u), "config.cfg"));
    std::memcpy(block, text, sizeBytes + 1u);
    std::free(text);
    return block;
}

} // extern "C"

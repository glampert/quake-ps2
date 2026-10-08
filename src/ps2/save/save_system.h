#pragma once
/* ================================================================================================
 * File: save_system.h
 * Brief: Internals shared by the save game backend (src/ps2/save/).
 *
 *  QuakeSpasm writes a save game as one text file, <gamedir>/<name>.sav, through stdio, and its
 *  load and save menus read each slot's comment line back the same way. Running from the
 *  emulator's host: filesystem, that is where saves stay by default (ps2_savedevice "host"),
 *  exactly as QuakeSpasm keeps them. On a console - and under the emulator with
 *  ps2_savedevice "mc" - they go to the memory card instead:
 *
 *    engine (host_cmd.c, menu.c)
 *        |  PS2_SaveOpenWrite / PS2_SaveCloseWrite, PS2_SaveLoadText, PS2_SaveReadComment
 *        v
 *    packed blob (RAM)   the .sav text, deflated as the engine writes it
 *        |
 *        v
 *    slot archive        a header (with the menu comment) and the deflated .sav, CRC-checked,
 *        |               in one of the slot's two files: written A/B, so a failed write never
 *        v               loses the save before it
 *    device              the memory card in MEMORY CARD slot 1 (libmc)
 *
 *  packed_blob.cpp  - the deflating stdio stream, the blob it fills, and inflating it back
 *  slot_archive.cpp - the archive format, over any Device
 *  save_device.cpp  - error reporting and whole-file helpers the devices share
 *  memcard.cpp      - the memory card Device (libmc)
 *  mc_icon.cpp      - the icon.sys and 3D icon the PS2 browser shows for the save directory
 *  save_api.cpp     - the PS2_Save* and PS2_Config* hooks (engine_hooks.h), the ps2_savedevice
 *                     cvar, and where config.cfg is kept
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#include <cstdio>
#include <tamtypes.h>

namespace ps2::save {

// Registers the ps2_savedevice cvar and the ps2_saveinfo command. VID_Init calls it, with the
// backend's other commands (see ps2::sys::RegisterCommands).
void Init();

// What QuakeSpasm's CFG_ReadCvars does for the cvars read ahead of config.cfg running (the
// framebuffer format, the USB keyboard), but from the config.cfg PS2_ConfigLoadHunk would pick:
// the host file first under the emulator, the memory card's first on a console.
void ReadConfigCvars(const char ** vars, int numVars);

// Longest device file name, terminator included: the limit a memory card directory entry puts
// on a name (32 bytes with the terminator).
constexpr int kMaxNameLen = 32;

// ------------------------------------------------------------------------------------------------
// Error reporting
// ------------------------------------------------------------------------------------------------

// Records the one-line message the player is shown for the last failure and prints it to the
// console too. Any detail goes to the console through Con_Printf directly.
void SetError(const char * fmt, ...) Q_PRINTF_FUNC(1, 2);
void ClearError();
const char * LastError();

// CRC-32 (the zlib one, from miniz) of a byte range, continuing from `crc`. 0 to start.
u32 Crc32(u32 crc, const void * data, size_t sizeBytes);

// Copies a name into a fixed-size buffer, truncating to fit.
template<size_t N>
inline void CopyName(char (&dest)[N], const char * src)
{
    std::snprintf(dest, N, "%.*s", static_cast<int>(N - 1), src);
}

// ------------------------------------------------------------------------------------------------
// Device
// ------------------------------------------------------------------------------------------------

// A file in the game's save directory on a device.
struct DirEntry
{
    char name[kMaxNameLen];
    u32  sizeBytes;
};

enum class FileHandle : int
{
    Invalid = -1,
};

enum class OpenMode
{
    Read,
    Write,
};

// Somewhere save slots (and config.cfg) can be kept. Names are bare file names inside the
// device's save directory. One file is open at a time. The memory card is the one there is;
// the slot archive is written against this interface rather than against it.
class Device
{
public:
    // Checks the device can be used right now - the card is in, formatted, and so on - and
    // refreshes FreeBytes. On false, SetError has said why in words the player understands.
    virtual bool Probe() = 0;

    // A line for the console: where saves go and how much room is left, or why they can't.
    virtual const char * StatusText() = 0;

    // Room left, and the room a file of the given size takes up on it (allocation units,
    // directory entry). Only meaningful after a successful Probe.
    virtual u32 FreeBytes() const = 0;
    virtual u32 FileCostBytes(u32 sizeBytes) const = 0;

    // Creates the save directory (and whatever else it needs) if it isn't there yet.
    virtual bool EnsureSaveDir() = 0;

    // The files in the save directory; returns how many were stored, -1 on error.
    virtual int List(DirEntry * outEntries, int maxEntries) = 0;

    // Size of a file in the save directory; false if there is no such file.
    virtual bool FileSize(const char * name, u32 & outSizeBytes) = 0;

    // Whole-file I/O helpers are built from these. Open returns a handle >= 0, or FileHandle::Invalid.
    virtual FileHandle Open(const char * name, OpenMode mode) = 0;
    virtual bool Read(FileHandle handle, void * dest, u32 sizeBytes) = 0;
    virtual bool Write(FileHandle handle, const void * src, u32 sizeBytes) = 0;
    virtual bool Close(FileHandle handle) = 0; // False if the data may not have reached the device.
    virtual bool Delete(const char * name) = 0; // True if the file is gone, missing included.

    // Human-readable path of a file, for console messages.
    virtual const char * Describe(const char * name) = 0;

protected:
    ~Device() = default;
};

// Writes a whole file into the device's save directory, replacing any file of that name.
// False if it couldn't all be written.
bool WriteWholeFile(Device & device, const char * name, const void * data, u32 sizeBytes);

// Whether the device's save directory holds exactly these bytes under that name.
bool FileMatches(Device & device, const char * name, const void * expected, u32 sizeBytes);

} // namespace ps2::save

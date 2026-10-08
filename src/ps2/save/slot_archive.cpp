/* ================================================================================================
 * File: slot_archive.cpp
 * Brief: Save slots - a deflated .sav written to a device as one archive file. See slot_archive.h.
 *
 *  An archive is a header, then the save's deflated bytes, exactly as the packed blob holds them
 *  (no recompression, so writing one out is only I/O):
 *
 *      ArchiveHeader       magic, version, sequence, the menu comment, sizes, CRCs
 *      packed bytes        the .sav text, raw deflate
 *
 *  The header's CRC covers the header, packedCrc the deflated bytes and rawCrc the text they
 *  inflate to, and the file must be exactly as long as the header says: a copy that was cut
 *  short or whose sectors went bad is recognised as such before any of it reaches the game.
 *
 *  The memory card's ROM driver can't rename a file, so a slot can't be replaced by writing a
 *  temporary and renaming it. Instead it has two files,
 *  <slot>_a.q1s and <slot>_b.q1s: a save goes to the one not holding the newest good copy,
 *  numbered one higher, and only once it is complete is the other deleted. A reader takes the
 *  good copy with the highest number. If the device is interrupted mid-write, the previous save
 *  is still there. The Quake II port's slots, which held a whole directory of files, worked the
 *  same way.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/slot_archive.h"

#include <cstddef>
#include <cstring>

namespace ps2::save {
namespace {

constexpr u32 kArchiveMagic   = 0x53503151u; // "Q1PS"
constexpr u32 kArchiveVersion = 1;

struct ArchiveHeader
{
    u32  magic;
    u32  version;
    u32  headerBytes; // sizeof(ArchiveHeader)
    u32  sequence;    // Which of the slot's two copies is newer: the higher.
    u32  rawBytes;    // The .sav text.
    u32  rawCrc;
    u32  packedBytes; // Its deflated bytes, which follow the header.
    u32  packedCrc;
    char comment[kCommentLen];
    u32  reserved[5];
    u32  headerCrc;   // Crc32 of everything above.
};
static_assert(sizeof(ArchiveHeader) == 96);

// Moves packed bytes from the device into the blob. Cache line aligned, since a memory card read
// is a DMA; the card device bounces through its own buffer regardless.
constexpr u32 kIoBufferBytes = 8u * 1024u;
alignas(64) static u8 s_ioBuffer[kIoBufferBytes];

// One of a slot's two files, as found on the device.
struct Copy
{
    char file[kMaxNameLen];
    bool present;
    bool valid; // The header checks out and the file is as long as it says.
    u32  fileBytes;
    ArchiveHeader header;
};

inline u32 HeaderCrc(const ArchiveHeader & header)
{
    return Crc32(0, &header, offsetof(ArchiveHeader, headerCrc));
}

inline u32 ToKb(const u32 bytes)
{
    return (bytes + 1023u) / 1024u;
}

bool ReadHeader(Device & device, const char * file, ArchiveHeader & outHeader)
{
    const FileHandle handle = device.Open(file, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    const bool ok = device.Read(handle, &outHeader, sizeof(outHeader));
    device.Close(handle);

    return ok && outHeader.magic == kArchiveMagic && outHeader.version == kArchiveVersion &&
           outHeader.headerBytes == sizeof(ArchiveHeader) && outHeader.headerCrc == HeaderCrc(outHeader) &&
           std::memchr(outHeader.comment, '\0', sizeof(outHeader.comment)) != nullptr;
}

void LoadCopies(Device & device, const char * slot, Copy (&copies)[2])
{
    for (int i = 0; i < 2; ++i)
    {
        Copy & copy = copies[i];
        std::snprintf(copy.file, sizeof(copy.file), "%s_%c.q1s", slot, (i == 0) ? 'a' : 'b');

        copy.fileBytes = 0;
        copy.present   = device.FileSize(copy.file, copy.fileBytes);
        copy.valid     = copy.present && ReadHeader(device, copy.file, copy.header) &&
                         copy.fileBytes == sizeof(ArchiveHeader) + copy.header.packedBytes;
    }
}

// The valid copies, newest first; returns how many there are.
int NewestFirst(Copy (&copies)[2], Copy * (&outOrder)[2])
{
    int count = 0;
    for (Copy & copy : copies)
    {
        if (copy.valid)
        {
            outOrder[count++] = &copy;
        }
    }

    if (count == 2 && outOrder[1]->header.sequence > outOrder[0]->header.sequence)
    {
        Copy * const newer = outOrder[1];
        outOrder[1] = outOrder[0];
        outOrder[0] = newer;
    }
    return count;
}

bool WriteArchive(Device & device, const char * file, const ArchiveHeader & header, const Blob & save)
{
    const FileHandle handle = device.Open(file, OpenMode::Write);
    if (handle == FileHandle::Invalid)
    {
        SetError("Could not write %s.", device.Describe(file));
        return false;
    }

    bool ok = device.Write(handle, &header, sizeof(header));
    for (const Chunk * chunk = save.head; ok && chunk != nullptr; chunk = chunk->next)
    {
        ok = device.Write(handle, chunk->Data(), chunk->used);
    }

    ok = device.Close(handle) && ok;
    if (!ok)
    {
        SetError("Could not write %s.", device.Describe(file));
    }
    return ok;
}

enum class ReadResult
{
    Ok,
    Damaged,
    OutOfMemory,
};

// Reads a copy's packed bytes into `outSave`, checking them on the way.
ReadResult ReadArchive(Device & device, const Copy & copy, Blob & outSave)
{
    const ArchiveHeader & header = copy.header;
    outSave = Blob{};

    const FileHandle handle = device.Open(copy.file, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return ReadResult::Damaged;
    }

    ArchiveHeader again;
    bool ok = device.Read(handle, &again, sizeof(again)) && std::memcmp(&again, &header, sizeof(header)) == 0;
    bool outOfMemory = false;
    u32 crc = 0;

    for (u32 left = header.packedBytes; ok && left != 0;)
    {
        const u32 n = (left < kIoBufferBytes) ? left : kIoBufferBytes;
        ok = device.Read(handle, s_ioBuffer, n);
        if (ok)
        {
            crc = Crc32(crc, s_ioBuffer, n);
            ok = BlobAppend(outSave, s_ioBuffer, n);
            outOfMemory = !ok;
        }
        left -= n;
    }

    device.Close(handle);

    if (!ok || crc != header.packedCrc)
    {
        BlobFree(outSave);
        return outOfMemory ? ReadResult::OutOfMemory : ReadResult::Damaged;
    }

    outSave.rawBytes = header.rawBytes;
    outSave.rawCrc   = header.rawCrc;
    return ReadResult::Ok;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

bool StoreSlot(Device & device, const char * slot, const char * comment, const Blob & save)
{
    // The save directory may cost room of its own (and a card its icon), so look at the free
    // space once it exists.
    if (!device.Probe() || !device.EnsureSaveDir() || !device.Probe())
    {
        return false;
    }

    Copy copies[2];
    LoadCopies(device, slot, copies);

    Copy * order[2] = {};
    const int numValid = NewestFirst(copies, order);
    const Copy * const newest = (numValid != 0) ? order[0] : nullptr;

    Copy & target = (newest  == &copies[0]) ? copies[1] : copies[0];
    Copy & other  = (&target == &copies[0]) ? copies[1] : copies[0];

    ArchiveHeader header = {};
    header.magic       = kArchiveMagic;
    header.version     = kArchiveVersion;
    header.headerBytes = sizeof(ArchiveHeader);
    header.sequence    = (newest != nullptr) ? newest->header.sequence + 1u : 1u;
    header.rawBytes    = save.rawBytes;
    header.rawCrc      = save.rawCrc;
    header.packedBytes = save.packedBytes;
    header.packedCrc   = BlobPackedCrc(save, 0);
    CopyName(header.comment, comment);
    header.headerCrc   = HeaderCrc(header);

    // Room for the new copy, counting the one it replaces (the older or a bad one). When there
    // is only room once the newest copy is gone too, it is overwritten in place instead.
    const u32 neededBytes = device.FileCostBytes(sizeof(ArchiveHeader) + save.packedBytes);
    const u32 availableBytes = device.FreeBytes() + (target.present ? device.FileCostBytes(target.fileBytes) : 0u);
    bool inPlace = false;

    if (availableBytes < neededBytes)
    {
        if (other.present && availableBytes + device.FileCostBytes(other.fileBytes) >= neededBytes)
        {
            inPlace = true;
            Con_Printf("Save: no room for a second copy of '%s' - replacing the old one first.\n", slot);
        }
        else
        {
            const u32 freeBytes = availableBytes + (other.present ? device.FileCostBytes(other.fileBytes) : 0u);
            SetError("Not enough free space: %u KB needed, %u KB free.",
                     static_cast<unsigned>(ToKb(neededBytes)), static_cast<unsigned>(ToKb(freeBytes)));
            return false;
        }
    }

    if ((inPlace && other.present && !device.Delete(other.file)) ||
        (target.present && !device.Delete(target.file)))
    {
        SetError("Could not replace the old save.");
        return false;
    }

    if (!WriteArchive(device, target.file, header, save))
    {
        device.Delete(target.file); // Don't leave half a copy behind, even though it would be ignored.
        return false;
    }

    if (!inPlace && other.present && !device.Delete(other.file))
    {
        Con_Printf("Save: couldn't delete the previous copy, %s; the new one is used regardless.\n",
                   device.Describe(other.file));
    }
    return true;
}

bool RestoreSlot(Device & device, const char * slot, Blob & outSave)
{
    if (!device.Probe())
    {
        return false;
    }

    Copy copies[2];
    LoadCopies(device, slot, copies);

    Copy * order[2] = {};
    const int numValid = NewestFirst(copies, order);
    if (numValid == 0)
    {
        SetError((copies[0].present || copies[1].present) ? "The save game is damaged." : "There is no saved game in that slot.");
        return false;
    }

    for (int i = 0; i < numValid; ++i)
    {
        const Copy & copy = *order[i];
        const ReadResult result = ReadArchive(device, copy, outSave);
        if (result == ReadResult::Ok)
        {
            return true;
        }
        if (result == ReadResult::OutOfMemory)
        {
            SetError("Not enough memory to load the game.");
            return false;
        }

        Con_Printf("Save: %s is damaged%s.\n", device.Describe(copy.file),
                   (i + 1 < numValid) ? " - trying the older copy" : "");
        SetError("The save game is damaged.");
    }
    return false;
}

SlotInfo ReadSlotInfo(Device & device, const char * slot)
{
    SlotInfo info = {};
    info.state = SlotState::Empty;

    Copy copies[2];
    LoadCopies(device, slot, copies);

    Copy * order[2] = {};
    if (NewestFirst(copies, order) != 0)
    {
        info.state = SlotState::Valid;
        CopyName(info.comment, order[0]->header.comment);
    }
    else if (copies[0].present || copies[1].present)
    {
        info.state = SlotState::Corrupt;
    }
    return info;
}

} // namespace ps2::save

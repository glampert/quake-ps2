#pragma once
/* ================================================================================================
 * File: slot_archive.h
 * Brief: Save slots: a deflated .sav written out to a device as an archive file, and read back.
 *        See save_system.h for where they sit, slot_archive.cpp for the format.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/packed_blob.h"

namespace ps2::save {

enum class SlotState
{
    Empty,
    Valid,
    Corrupt,
};

// QuakeSpasm's SAVEGAME_COMMENT_LENGTH (39), terminator included: the level name and kills
// line the menus list a slot by, with spaces as '_' (Host_SavegameComment).
constexpr int kCommentLen = 40;

struct SlotInfo
{
    SlotState state;
    char      comment[kCommentLen];
};

// Longest slot name: its files are "<slot>_a.q1s" and "<slot>_b.q1s", within kMaxNameLen.
constexpr int kMaxSlotNameLen = 24;

// Writes a save to the slot. Never loses what the slot held before unless the device has no
// room for two copies, and says so on the console when it has to.
bool StoreSlot(Device & device, const char * slot, const char * comment, const Blob & save);

// Reads the slot's newest good copy into `outSave`, which must be empty: the save deflated as it
// was written, checked against its CRC. False (SetError) leaves it empty.
bool RestoreSlot(Device & device, const char * slot, Blob & outSave);

// The slot as the menus list it, from its files' headers only.
SlotInfo ReadSlotInfo(Device & device, const char * slot);

} // namespace ps2::save

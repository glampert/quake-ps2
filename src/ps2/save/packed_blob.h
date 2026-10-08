#pragma once
/* ================================================================================================
 * File: packed_blob.h
 * Brief: A save game's text, deflated: written through a stdio stream that compresses as the
 *        engine writes, kept in RAM as a chain of chunks, and inflated back whole for the loader.
 *        See save_system.h for where it sits, packed_blob.cpp for the details.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/save_system.h"

#include <cstdio>

namespace ps2::save {

// A slice of a blob's deflated bytes; the payload follows the header in the same allocation.
struct Chunk
{
    Chunk * next;
    u32     used;     // Bytes of payload filled in.
    u32     capacity; // Bytes of payload allocated.

    u8 *       Data()       { return static_cast<u8 *>(static_cast<void *>(this + 1)); }
    const u8 * Data() const { return static_cast<const u8 *>(static_cast<const void *>(this + 1)); }
};

// A file's contents, deflated (raw deflate, no zlib wrapper), as a chain of chunks.
struct Blob
{
    Chunk * head        = nullptr;
    Chunk * tail        = nullptr;
    u32     packedBytes = 0;
    u32     rawBytes    = 0;
    u32     rawCrc      = 0; // Crc32 of the inflated contents.
};

// Appends bytes to the end of a blob. False, with the blob unchanged, if out of memory.
bool BlobAppend(Blob & blob, const void * data, u32 sizeBytes);

// Frees the chunks and resets the blob to empty.
void BlobFree(Blob & blob);

// Crc32 over the blob's packed bytes, continuing from `crc`.
u32 BlobPackedCrc(const Blob & blob, u32 crc);

// A stdio stream that deflates what is written to it into `out`, which must be empty and stay
// put until the stream is closed. Null (SetError) if out of memory.
std::FILE * OpenPackStream(Blob & out);

// Closes a stream from OpenPackStream. True if everything written made it into the blob, which
// then holds it, sizes and CRC included; false (SetError) leaves the blob empty.
bool ClosePackStream(std::FILE * stream);

// Inflates a whole blob into a new malloc block - as QuakeSpasm's file loaders return text, so it
// frees it as one of theirs - with a 0 after the blob's rawBytes. Null (SetError) if out of
// memory, or if the data doesn't inflate to exactly the size and CRC recorded with it.
char * InflateBlobToText(const Blob & blob);

} // namespace ps2::save

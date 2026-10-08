/* ================================================================================================
 * File: packed_blob.cpp
 * Brief: The deflated save text. See packed_blob.h.
 *
 *  Writing compresses as the engine writes: the stream comes from fopencookie, so the fprintfs
 *  of Host_Savegame_f work on it unchanged, and what they write is deflated as it arrives (miniz
 *  tdefl, with an output callback appending to the blob's chunks). The text is never held whole,
 *  only its deflated form, a tenth of it or less. Only the compressor state is large (164 KB),
 *  and only while a save is being written.
 *
 *  Reading inflates the whole blob in one go into the buffer the loader parses, which is all of
 *  the text anyway.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

// newlib only declares fopencookie for GNU sources, and has to see this before any header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "ps2/save/packed_blob.h"

#include <cstdlib>
#include <cstring>
#include <miniz.h>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;

// Chunks start small and double up to this.
constexpr u32 kFirstChunkBytes = 1024u;
constexpr u32 kMaxChunkBytes   = 16u * 1024u;

// A chunk left with more slack than this when its blob is complete is copied to a tight one.
constexpr u32 kMaxChunkSlackBytes = 512u;

// Deflate level (0-10). A save is text - field names and numbers, over and over - which every
// level compresses well; this one trades a little speed for a smaller file on the card.
constexpr int kDeflateLevel = 3;

// stdio buffering on the stream: ED_Write fflushes after every entity, so this is at most one
// entity's worth of text reaching tdefl at a time.
constexpr size_t kStreamBufferBytes = 8u * 1024u;

Chunk * AllocChunk(const u32 capacity)
{
    void * mem = ps2::heap::TryAlloc(sizeof(Chunk) + capacity, MemTag::SaveData);
    if (mem == nullptr)
    {
        return nullptr;
    }

    Chunk * chunk  = static_cast<Chunk *>(mem);
    chunk->next     = nullptr;
    chunk->used     = 0;
    chunk->capacity = capacity;
    return chunk;
}

void FreeChunk(Chunk * chunk)
{
    ps2::heap::Free(chunk, sizeof(Chunk) + chunk->capacity, MemTag::SaveData);
}

// Swaps the tail chunk for one exactly its size, if it was left with a lot of room.
void TrimBlob(Blob & blob)
{
    Chunk * const tail = blob.tail;
    if (tail == nullptr || (tail->capacity - tail->used) <= kMaxChunkSlackBytes)
    {
        return;
    }

    Chunk * const tight = AllocChunk(tail->used);
    if (tight == nullptr)
    {
        return; // Wasteful, not wrong.
    }

    std::memcpy(tight->Data(), tail->Data(), tail->used);
    tight->used = tail->used;

    if (blob.head == tail)
    {
        blob.head = tight;
    }
    else
    {
        Chunk * prev = blob.head;
        while (prev->next != tail)
        {
            prev = prev->next;
        }
        prev->next = tight;
    }
    blob.tail = tight;
    FreeChunk(tail);
}

// ------------------------------------------------------------------------------------------------
// The write stream
// ------------------------------------------------------------------------------------------------

struct PackStream
{
    tdefl_compressor * compressor;
    Blob * out;
    u32    crc;
    bool   failed;
};

mz_bool PutDeflated(const void * data, int sizeBytes, void * user)
{
    PackStream & stream = *static_cast<PackStream *>(user);
    return BlobAppend(*stream.out, data, static_cast<u32>(sizeBytes)) ? MZ_TRUE : MZ_FALSE;
}

ssize_t PackStreamWrite(void * cookie, const char * data, size_t sizeBytes)
{
    PackStream & stream = *static_cast<PackStream *>(cookie);
    if (stream.failed)
    {
        return -1;
    }

    stream.crc = Crc32(stream.crc, data, sizeBytes);
    stream.out->rawBytes += static_cast<u32>(sizeBytes);

    if (tdefl_compress_buffer(stream.compressor, data, sizeBytes, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY)
    {
        SetError("Not enough memory to save the game.");
        stream.failed = true;
        return -1;
    }
    return static_cast<ssize_t>(sizeBytes);
}

int PackStreamClose(void * cookie)
{
    PackStream * const stream = static_cast<PackStream *>(cookie);
    bool ok = !stream->failed;

    if (ok && tdefl_compress_buffer(stream->compressor, nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE)
    {
        SetError("Not enough memory to save the game.");
        ok = false;
    }

    if (ok)
    {
        TrimBlob(*stream->out);
        stream->out->rawCrc = stream->crc;
    }
    else
    {
        BlobFree(*stream->out);
    }

    ps2::heap::Free(stream->compressor, sizeof(tdefl_compressor), MemTag::SaveData);
    ps2::heap::Free(stream, sizeof(PackStream), MemTag::SaveData);
    return ok ? 0 : -1;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Blobs
// ------------------------------------------------------------------------------------------------

bool BlobAppend(Blob & blob, const void * data, const u32 sizeBytes)
{
    const u8 * src = static_cast<const u8 *>(data);
    u32 left = sizeBytes;

    // Room in the tail first, then new chunks, each double the last up to the cap. Chunks
    // are only linked in once all of them exist, so a failure leaves the blob as it was.
    Chunk * tail = blob.tail;
    const u32 tailRoom = (tail != nullptr) ? (tail->capacity - tail->used) : 0u;
    const u32 intoTail = (left < tailRoom) ? left : tailRoom;
    left -= intoTail;

    Chunk * newHead = nullptr;
    Chunk * newTail = nullptr;
    u32 nextCapacity = (tail != nullptr) ? tail->capacity : (kFirstChunkBytes / 2u);
    for (u32 remaining = left; remaining != 0;)
    {
        nextCapacity = (nextCapacity * 2u < kMaxChunkBytes) ? nextCapacity * 2u : kMaxChunkBytes;
        Chunk * const chunk = AllocChunk(nextCapacity);
        if (chunk == nullptr)
        {
            while (newHead != nullptr)
            {
                Chunk * const next = newHead->next;
                FreeChunk(newHead);
                newHead = next;
            }
            return false;
        }

        if (newTail != nullptr) { newTail->next = chunk; } else { newHead = chunk; }
        newTail = chunk;
        remaining -= (remaining < nextCapacity) ? remaining : nextCapacity;
    }

    if (intoTail != 0)
    {
        std::memcpy(tail->Data() + tail->used, src, intoTail);
        tail->used += intoTail;
        src += intoTail;
    }

    for (Chunk * chunk = newHead; chunk != nullptr; chunk = chunk->next)
    {
        const u32 n = (left < chunk->capacity) ? left : chunk->capacity;
        std::memcpy(chunk->Data(), src, n);
        chunk->used = n;
        src  += n;
        left -= n;
    }

    if (newHead != nullptr)
    {
        if (tail != nullptr) { tail->next = newHead; } else { blob.head = newHead; }
        blob.tail = newTail;
    }

    blob.packedBytes += sizeBytes;
    return true;
}

void BlobFree(Blob & blob)
{
    for (Chunk * chunk = blob.head; chunk != nullptr;)
    {
        Chunk * const next = chunk->next;
        FreeChunk(chunk);
        chunk = next;
    }
    blob = Blob{};
}

u32 BlobPackedCrc(const Blob & blob, u32 crc)
{
    for (const Chunk * chunk = blob.head; chunk != nullptr; chunk = chunk->next)
    {
        crc = Crc32(crc, chunk->Data(), chunk->used);
    }
    return crc;
}

// ------------------------------------------------------------------------------------------------
// Streams
// ------------------------------------------------------------------------------------------------

std::FILE * OpenPackStream(Blob & out)
{
    PS2_Assert(out.head == nullptr);

    PackStream * const stream = static_cast<PackStream *>(ps2::heap::TryAlloc(sizeof(PackStream), MemTag::SaveData));
    tdefl_compressor * const compressor = static_cast<tdefl_compressor *>(ps2::heap::TryAlloc(sizeof(tdefl_compressor), MemTag::SaveData));

    if (stream == nullptr || compressor == nullptr)
    {
        ps2::heap::Free(stream, sizeof(PackStream), MemTag::SaveData);
        ps2::heap::Free(compressor, sizeof(tdefl_compressor), MemTag::SaveData);
        SetError("Not enough memory to save the game.");
        return nullptr;
    }

    out                = Blob{};
    stream->compressor = compressor;
    stream->out        = &out;
    stream->crc        = 0;
    stream->failed     = false;

    // Raw deflate: the blob carries its own size and CRC.
    const mz_uint flags = tdefl_create_comp_flags_from_zip_params(kDeflateLevel, -15, MZ_DEFAULT_STRATEGY);
    tdefl_init(compressor, &PutDeflated, stream, static_cast<int>(flags));

    cookie_io_functions_t io = {};
    io.write = &PackStreamWrite;
    io.close = &PackStreamClose;

    std::FILE * const file = fopencookie(stream, "w", io);
    if (file == nullptr)
    {
        ps2::heap::Free(compressor, sizeof(tdefl_compressor), MemTag::SaveData);
        ps2::heap::Free(stream, sizeof(PackStream), MemTag::SaveData);
        SetError("Not enough memory to save the game.");
        return nullptr;
    }

    std::setvbuf(file, nullptr, _IOFBF, kStreamBufferBytes);
    return file;
}

bool ClosePackStream(std::FILE * stream)
{
    // fclose fails if the final flush does, or the close function (PackStreamClose) does.
    return std::fclose(stream) == 0;
}

char * InflateBlobToText(const Blob & blob)
{
    char * const text = static_cast<char *>(std::malloc(static_cast<size_t>(blob.rawBytes) + 1u));
    tinfl_decompressor * const inflator =
        static_cast<tinfl_decompressor *>(ps2::heap::TryAlloc(sizeof(tinfl_decompressor), MemTag::SaveData));

    if (text == nullptr || inflator == nullptr)
    {
        std::free(text);
        ps2::heap::Free(inflator, sizeof(tinfl_decompressor), MemTag::SaveData);
        SetError("Not enough memory to load the game.");
        return nullptr;
    }

    // The whole text is one buffer, so tinfl can look back into it for its matches: no
    // dictionary of its own needed.
    tinfl_init(inflator);
    mz_uint8 * const out = static_cast<mz_uint8 *>(static_cast<void *>(text));
    const Chunk * chunk = blob.head;
    u32 chunkOffset = 0;
    size_t outPos = 0;
    const char * damage = nullptr;

    for (;;)
    {
        const mz_uint8 * input = (chunk != nullptr) ? chunk->Data() + chunkOffset : nullptr;
        size_t inputBytes = (chunk != nullptr) ? chunk->used - chunkOffset : 0u;
        size_t outputBytes = blob.rawBytes - outPos;
        const bool moreInput = (chunk != nullptr && chunk->next != nullptr);

        const mz_uint32 flags = static_cast<mz_uint32>(TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) |
                                (moreInput ? static_cast<mz_uint32>(TINFL_FLAG_HAS_MORE_INPUT) : 0u);
        const tinfl_status status = tinfl_decompress(inflator, input, &inputBytes, out, out + outPos, &outputBytes, flags);

        chunkOffset += static_cast<u32>(inputBytes);
        outPos      += outputBytes;
        if (chunk != nullptr && chunkOffset == chunk->used)
        {
            chunk       = chunk->next;
            chunkOffset = 0;
        }

        if (status == TINFL_STATUS_DONE)
        {
            break;
        }
        if (status < TINFL_STATUS_DONE)
        {
            damage = "bad deflate data";
            break;
        }
        if (status == TINFL_STATUS_HAS_MORE_OUTPUT)
        {
            damage = "longer than recorded";
            break;
        }
        if (inputBytes == 0 && outputBytes == 0 && chunk == nullptr)
        {
            damage = "truncated";
            break;
        }
    }

    ps2::heap::Free(inflator, sizeof(tinfl_decompressor), MemTag::SaveData);

    if (damage == nullptr && outPos != blob.rawBytes)
    {
        damage = "shorter than recorded";
    }
    if (damage == nullptr && Crc32(0, text, outPos) != blob.rawCrc)
    {
        damage = "CRC mismatch";
    }

    if (damage != nullptr)
    {
        std::free(text);
        Con_Printf("Save: the saved game doesn't inflate (%s).\n", damage);
        SetError("The save game is damaged.");
        return nullptr;
    }

    text[outPos] = '\0';
    return text;
}

} // namespace ps2::save

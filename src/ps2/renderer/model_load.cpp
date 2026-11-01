/* ================================================================================================
 * File: model_load.cpp
 * Brief: Background alias model reloads. See model_load.h.
 *
 *  A small table of reads in flight, each with the model it is for, its staging buffer (64-byte
 *  aligned, the file rounded up, tag Aio) and the file's location. The file is read or inflated
 *  whole and converted from memory, the Quake II port's way: a pkz entry is deflated, so it has no
 *  ranges to read in place, and pak0.pkz is what a USB stick carries. Everything here runs on the
 *  main thread; the loader only ever sees the staging buffer and the descriptor.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/model_load.h"
#include "ps2/fs/async_io.h"
#include "ps2/system/heap.h"
#include "ps2/system/sys.h"
#include "ps2/debug/profile.h"    // ReadCycles
#include "ps2/renderer/profile.h" // FrameLogNoteReady
#include "ps2/renderer/loading_screen.h"

#include <cstring>

namespace ps2::mod {
namespace {

constexpr int kMaxPending = 8;

// The largest file reloaded in the background: the staging copy and the model's hunk and cache
// copies are all live at the finish. id's largest alias model (progs/player.mdl) is 190 KB.
constexpr int kMaxAsyncModelBytes = 768 * 1024;

struct PendingModel
{
    qmodel_t *    mod = nullptr; // null: the entry is free
    aio::Ticket   ticket;
    fs_location_t loc;
    unsigned int  pathId       = 0;
    u8 *          staging      = nullptr;
    u32           stagingBytes = 0;
    int           length       = 0;
    int           submitMs     = 0;
};

static PendingModel s_pending[kMaxPending];
static int          s_numPending = 0;

// The read is over, one way or the other: converts the model from the staging buffer, or reports,
// and frees the entry.
void Finish(PendingModel & p)
{
    const aio::State state  = aio::Poll(p.ticket);
    const int        result = aio::Result(p.ticket);

    if (state == aio::State::Done && result == p.length) [[likely]]
    {
        PS2_ModelFinishLoad(p.mod, p.staging, p.length, p.pathId);
        Con_DPrintf("model: %s reloaded in the background, %d ms after it was asked for.\n", p.mod->name,
                    sys::Milliseconds() - p.submitMs);
    }
    else
    {
        // Mod_Extradata asks again at the next draw, synchronously, and reports what it finds.
        Con_Printf("WARNING: %s: background read failed (%d).\n", p.mod->name, result);
    }

    aio::Release(p.ticket);
    heap::Free(p.staging, p.stagingBytes, heap::MemTag::Aio);
    COM_ReleaseLocation(&p.loc);
    debug::FrameLogNoteReady(p.mod->name);

    p.mod     = nullptr;
    p.staging = nullptr;

    PS2_Assert(s_numPending > 0);
    --s_numPending;
}

} // namespace

int PumpLoads(const u32 budgetCycles)
{
    if (s_numPending == 0)
    {
        return 0;
    }

    const debug::CpuCycles start = debug::ReadCycles();
    int finished = 0;
    for (PendingModel & p : s_pending)
    {
        if (p.mod == nullptr)
        {
            continue;
        }
        const aio::State state = aio::Poll(p.ticket);
        if (state != aio::State::Done && state != aio::State::Failed)
        {
            continue;
        }
        Finish(p);
        ++finished;
        if (debug::ReadCycles() - start > budgetCycles)
        {
            break;
        }
    }
    return finished;
}

void DrainLoads()
{
    for (PendingModel & p : s_pending)
    {
        if (p.mod != nullptr)
        {
            aio::Wait(p.ticket);
            Finish(p);
        }
    }
}

} // namespace ps2::mod

// ------------------------------------------------------------------------------------------------
// C API
// ------------------------------------------------------------------------------------------------

using namespace ps2;
using namespace ps2::mod;

extern "C" int PS2_ModelLoadAsync(qmodel_t * const mod)
{
    if (!aio::Enabled() || loading::Shown() || s_numPending == kMaxPending)
    {
        return 0;
    }

    PendingModel * entry = nullptr;
    for (PendingModel & p : s_pending)
    {
        if (p.mod == nullptr)
        {
            entry = &p;
            break;
        }
    }
    if (entry == nullptr)
    {
        return 0;
    }

    fs_location_t loc;
    unsigned int pathId = 0;
    const int length = COM_LocateFile(mod->name, &loc, &pathId);
    if (length <= 0 || loc.fd < 0 || length > kMaxAsyncModelBytes)
    {
        if (length >= 0)
        {
            COM_ReleaseLocation(&loc);
        }
        return 0; // missing, empty, unlocatable or too large: the synchronous path, as always
    }

    const u32 stagingBytes = aio::RoundUp64(static_cast<u32>(length));
    u8 * const staging     = static_cast<u8 *>(heap::TryAllocAligned(heap::MemAlign(64), stagingBytes, heap::MemTag::Aio));
    if (staging == nullptr)
    {
        COM_ReleaseLocation(&loc);
        return 0;
    }

    aio::Request request;
    request.kind      = (loc.kind == FS_LOC_PKZ_DEFLATED) ? aio::Kind::InflateWhole : aio::Kind::Read;
    request.prio      = aio::Priority::Asset;
    request.fd        = loc.fd;
    request.offset    = loc.offset;
    request.bytes     = loc.bytes;
    request.dest      = staging;
    request.destBytes = stagingBytes;
    request.rawBytes  = loc.rawBytes;

    const aio::Ticket ticket = aio::Submit(request);
    if (!ticket.IsValid())
    {
        heap::Free(staging, stagingBytes, heap::MemTag::Aio);
        COM_ReleaseLocation(&loc);
        return 0;
    }

    entry->mod          = mod;
    entry->ticket       = ticket;
    entry->loc          = loc;
    entry->pathId       = pathId;
    entry->staging      = staging;
    entry->stagingBytes = stagingBytes;
    entry->length       = length;
    entry->submitMs     = sys::Milliseconds();
    ++s_numPending;
    return 1;
}

extern "C" int PS2_ModelLoadPending(const qmodel_t * const mod)
{
    if (s_numPending == 0)
    {
        return 0;
    }
    for (const PendingModel & p : s_pending)
    {
        if (p.mod == mod)
        {
            return 1;
        }
    }
    return 0;
}

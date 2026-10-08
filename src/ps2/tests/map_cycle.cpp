/* ================================================================================================
 * File: map_cycle.cpp
 * Brief: Map cycling memory smoke test. See map_cycle.h.
 *
 *  Drives the real console command ("map <name>") through the command buffer rather than
 *  calling into the server directly, so the sequence the test exercises is byte for byte the
 *  one a player produces.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#if PS2_QUAKE_DEBUG
#include "ps2/tests/map_cycle.h"
#include "ps2/system/heap.h"
#include "ps2/system/sys.h"

#include <cstdio>
#include <cstring>

extern "C" {
// zone.c's hunk bookkeeping, which zone.h doesn't declare.
extern int hunk_size;
extern int hunk_low_used;
extern int hunk_high_used;
}

namespace ps2::test {
namespace {

// Every map of the full game, in the order a playthrough meets them: start, then each
// episode's levels and its secret one. A map the game data doesn't have (the shareware pak
// has start and episode 1) is skipped.
constexpr const char * kMaps[] = {
    "start",
    "e1m1", "e1m2", "e1m3", "e1m4", "e1m5", "e1m6", "e1m7", "e1m8",
    "e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7",
    "e3m1", "e3m2", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7",
    "e4m1", "e4m2", "e4m3", "e4m4", "e4m5", "e4m6", "e4m7", "e4m8",
    "end",
};

enum class State
{
    Idle,    // Nothing issued yet; kick off the next map.
    Loading, // Command issued, waiting for the world to come up.
    Dwelling // Map is up; stay in it so it actually renders.
};

// A map that never comes up is a failed test, not a reason to hang forever.
constexpr int kLoadTimeoutMs = 90 * 1000;

// The level has to be up for this many frames before it counts as loaded: it guards the window
// between Cbuf_AddText and the command running, where the previous map is still up.
constexpr int kFramesToConfirm = 2;

static cvar_t s_enabled = ps2::MakeCvar("ps2_testmaps", "0", CVAR_NONE);
static cvar_t s_dwell   = ps2::MakeCvar("ps2_testmaps_dwell", "8", CVAR_NONE);

static State  s_state         = State::Idle;
static int    s_nextMap       = 0;
static bool   s_done          = false;
static int    s_issuedAtMs    = 0;
static int    s_dwellUntilMs  = 0;
static int    s_confirmFrames = 0;
static int    s_skipped       = 0;
static int    s_failed        = 0;
static size_t s_peakBeforeMap = 0;
static int    s_hunkPeak      = 0; // Hunk in use plus cache, the most any map took.
static char   s_targetBsp[MAX_QPATH] = {};

void Restart()
{
    s_state         = State::Idle;
    s_nextMap       = 0;
    s_done          = false;
    s_issuedAtMs    = 0;
    s_dwellUntilMs  = 0;
    s_confirmFrames = 0;
    s_skipped       = 0;
    s_failed        = 0;
    s_peakBeforeMap = 0;
    s_hunkPeak      = 0;
    s_targetBsp[0]  = '\0';
}

bool TargetLevelIsUp()
{
    return cls.signon == SIGNONS && cl.worldmodel != nullptr && std::strcmp(cl.worldmodel->name, s_targetBsp) == 0;
}

size_t TagBytes(const ps2::heap::MemTag tag)
{
    return ps2::heap::GetStatsForMemTag(tag).totalBytes;
}

// One line per map: QuakeSpasm's hunk (the level, and the cache of models and sounds it loaded,
// and what is left of the hunk after both), then the backend's level data by tag and the
// program's totals. "NEW PEAK" marks the map whose load took the most of the heap.
void ReportMap(const char * const name, const int index)
{
    using ps2::heap::FormatMemoryUnit;
    using ps2::heap::MemTag;
    constexpr size_t kUnit = ps2::heap::kMemUnitStrSize;

    const int hunkUsed  = hunk_low_used + hunk_high_used;
    const int cacheUsed = Cache_UsedBytes();
    const int hunkLeft  = hunk_size - hunkUsed - cacheUsed;
    s_hunkPeak = (hunkUsed + cacheUsed > s_hunkPeak) ? hunkUsed + cacheUsed : s_hunkPeak;

    char hunk[kUnit], cache[kUnit], left[kUnit];
    char world[kUnit], light[kUnit], tex[kUnit], alias[kUnit], music[kUnit];
    char peak[kUnit], freeMem[kUnit];

    const size_t peakNow = ps2::heap::GetPeakMemBytes();

    Con_Printf("MapCycle [%2d/%2d] %-6s Hunk %-9s Cache %-9s HunkLeft %-9s | World %-9s Light %-9s Tex %-9s "
               "Mdl %-9s Mus %-9s | PEAK %-9s FREE %-9s%s\n",
               index + 1, ps2::ArrayLength(kMaps), name,
               FormatMemoryUnit(static_cast<size_t>(hunkUsed),  true, hunk,  sizeof(hunk)),
               FormatMemoryUnit(static_cast<size_t>(cacheUsed), true, cache, sizeof(cache)),
               FormatMemoryUnit(static_cast<size_t>((hunkLeft > 0) ? hunkLeft : 0), true, left, sizeof(left)),
               FormatMemoryUnit(TagBytes(MemTag::WorldMdl), true, world, sizeof(world)),
               FormatMemoryUnit(TagBytes(MemTag::Lightmap), true, light, sizeof(light)),
               FormatMemoryUnit(TagBytes(MemTag::TexImage), true, tex,   sizeof(tex)),
               FormatMemoryUnit(TagBytes(MemTag::AliasMdl), true, alias, sizeof(alias)),
               FormatMemoryUnit(TagBytes(MemTag::Music),    true, music, sizeof(music)),
               FormatMemoryUnit(peakNow,                    true, peak,  sizeof(peak)),
               FormatMemoryUnit(ps2::heap::GetAvailableMemBytes(), true, freeMem, sizeof(freeMem)),
               (peakNow > s_peakBeforeMap) ? "  <- NEW PEAK" : "");

    Con_Printf("MapCycle [%2d/%2d] %-6s load peak %-9s ARENA %-9s\n",
               index + 1, ps2::ArrayLength(kMaps), name,
               FormatMemoryUnit(ps2::heap::GetWindowPeakMemBytes(), true, peak, sizeof(peak)),
               FormatMemoryUnit(ps2::heap::GetHeapStats().arenaBytes, true, freeMem, sizeof(freeMem)));
}

// Where the free memory sits, which the memtag table cannot show: dlmalloc never moves a live
// block, so what matters is not how much is free but how it is arranged. See the Quake II
// port's notes on it; a number that climbs pass over pass is the heap degrading.
void ReportHeap(const int pass)
{
    const ps2::heap::HeapStats hs = ps2::heap::GetHeapStats();
    char a[ps2::heap::kMemUnitStrSize], b[ps2::heap::kMemUnitStrSize], c[ps2::heap::kMemUnitStrSize];

    // The top chunk is one contiguous run at the end of the arena, and fastbins are small chunks
    // dlmalloc leaves uncoalesced on purpose. Neither is fragmentation; what is left over is.
    const size_t nonInterior    = hs.topChunkBytes + hs.fastbinBytes;
    const size_t interior       = (hs.freeBytes > nonInterior) ? (hs.freeBytes - nonInterior) : 0u;
    const size_t interiorChunks = (hs.freeChunks > 1u) ? (hs.freeChunks - 1u) : 0u;

    Con_Printf("MapCycle: ---- heap after pass %d ----\n", pass);
    Con_Printf("MapCycle:   arena %s   in use %s   free %s\n",
               ps2::heap::FormatMemoryUnit(hs.arenaBytes, true, a, sizeof(a)),
               ps2::heap::FormatMemoryUnit(hs.inUseBytes, true, b, sizeof(b)),
               ps2::heap::FormatMemoryUnit(hs.freeBytes,  true, c, sizeof(c)));
    Con_Printf("MapCycle:   top chunk %s   fastbins %s in %u\n",
               ps2::heap::FormatMemoryUnit(hs.topChunkBytes, true, a, sizeof(a)),
               ps2::heap::FormatMemoryUnit(hs.fastbinBytes,  true, b, sizeof(b)),
               static_cast<unsigned>(hs.fastbinChunks));
    Con_Printf("MapCycle:   interior holes %s in %u chunks\n",
               ps2::heap::FormatMemoryUnit(interior, true, a, sizeof(a)), static_cast<unsigned>(interiorChunks));
}

void Finish()
{
    char peak[ps2::heap::kMemUnitStrSize], total[ps2::heap::kMemUnitStrSize];
    char hunk[ps2::heap::kMemUnitStrSize], hunkSize[ps2::heap::kMemUnitStrSize];

    Con_Printf("MapCycle: pass complete - %d loaded, %d skipped (not in the game data), %d timed out.\n",
               ps2::ArrayLength(kMaps) - s_skipped - s_failed, s_skipped, s_failed);
    Con_Printf("MapCycle: the heap peaked at %s of %s installed; the most hunk any map took, cache included, "
               "was %s of %s.\n",
               ps2::heap::FormatMemoryUnit(ps2::heap::GetPeakMemBytes(), true, peak, sizeof(peak)),
               ps2::heap::FormatMemoryUnit(ps2::heap::GetTotalMemBytes(), true, total, sizeof(total)),
               ps2::heap::FormatMemoryUnit(static_cast<size_t>(s_hunkPeak), true, hunk, sizeof(hunk)),
               ps2::heap::FormatMemoryUnit(static_cast<size_t>(hunk_size), true, hunkSize, sizeof(hunkSize)));

    // Survives Restart(), so re-running the test in the same session numbers the passes and
    // makes drift between them obvious.
    static int s_passesRun = 0;
    ReportHeap(++s_passesRun);
    Con_Printf("MapCycle: done.\n");

    s_done = true;
}

// Issues the next map, skipping any the game data doesn't have. Returns false when the list is
// exhausted.
bool StartNextMap()
{
    while (s_nextMap < ps2::ArrayLength(kMaps))
    {
        const char * const name = kMaps[s_nextMap];
        std::snprintf(s_targetBsp, sizeof(s_targetBsp), "maps/%s.bsp", name);

        if (!COM_FileExists(s_targetBsp, nullptr))
        {
            ++s_skipped;
            ++s_nextMap;
            continue;
        }

        // Sampled before the load so ReportMap can tell whether this map set a new high-water,
        // and the window peak restarted, so it measures this load alone.
        s_peakBeforeMap = ps2::heap::GetPeakMemBytes();
        ps2::heap::ResetWindowPeak();

        Cbuf_AddText(va("map %s\n", name));

        s_issuedAtMs    = ps2::sys::Milliseconds();
        s_confirmFrames = 0;
        s_state         = State::Loading;
        return true;
    }
    return false;
}

} // namespace

void InitMapCycle()
{
    Cvar_RegisterVariable(&s_enabled);
    Cvar_RegisterVariable(&s_dwell);
    Cmd_AddCommand("ps2_testmaps_restart", &Restart);
}

void RunMapCycle()
{
    if (s_enabled.value == 0.0f || s_done)
    {
        return;
    }

    switch (s_state)
    {
    case State::Idle:
        if (!StartNextMap())
        {
            Finish();
        }
        break;

    case State::Loading:
        if (TargetLevelIsUp() && ++s_confirmFrames >= kFramesToConfirm)
        {
            const int dwellMs = static_cast<int>(s_dwell.value * 1000.0f);
            s_dwellUntilMs = ps2::sys::Milliseconds() + ((dwellMs > 0) ? dwellMs : 1);
            s_state = State::Dwelling;
            break;
        }
        if ((ps2::sys::Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Con_Printf("MapCycle: '%s' never came up after %d seconds - moving on.\n",
                       kMaps[s_nextMap], kLoadTimeoutMs / 1000);
            ++s_failed;
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;

    case State::Dwelling:
        if (ps2::sys::Milliseconds() >= s_dwellUntilMs)
        {
            ReportMap(kMaps[s_nextMap], s_nextMap);
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG

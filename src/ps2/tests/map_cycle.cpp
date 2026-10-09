/* ================================================================================================
 * File: map_cycle.cpp
 * Brief: Map cycling memory and performance test. See map_cycle.h.
 *
 *  Drives the real console commands ("map <name>", and for the perf pass "setpos", "god" and
 *  "notarget") through the command buffer rather than calling into the server directly, so the
 *  sequence the test exercises is byte for byte the one a player produces. The perf pass's
 *  turns are the one exception: it sets the client's view angles, as a held stick would.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

#if PS2_QUAKE_DEBUG
#include "ps2/tests/map_cycle.h"
#include "ps2/tests/perf_run.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/heap.h"
#include "ps2/system/sys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
// zone.c's hunk bookkeeping, which zone.h doesn't declare.
extern int hunk_size;
extern int hunk_low_used;
extern int hunk_high_used;
}

namespace ps2::test {
namespace {

// The campaign, in the order a playthrough meets its maps: start, then each episode's levels and
// its secret one, then end. The shareware pak0 has start and episode 1; the registered pak1 adds
// the rest, and the deathmatch arenas, which the cycle visits after these.
constexpr const char * kCampaign[] = {
    "start",
    "e1m1", "e1m2", "e1m3", "e1m4", "e1m5", "e1m6", "e1m7", "e1m8",
    "e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7",
    "e3m1", "e3m2", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7",
    "e4m1", "e4m2", "e4m3", "e4m4", "e4m5", "e4m6", "e4m7", "e4m8",
    "end",
};

// A level the cycle visits, and the pak it came from, for the report.
struct MapEntry
{
    char name[32]; // as the "map" command takes it: "e2m1", not "maps/e2m1.bsp"
    char pak[16];  // the pak's file name without its extension: "pak1"
};

// Room for every map of id's data (38 with the registered pak) and then some; a pak with more
// levels than this has the rest left out, which the cycle says when it starts.
constexpr int kMaxMaps = 128;

// QuakeSpasm's own test for a level in a pak (ExtraMaps_Init): a .bsp under maps/ bigger than
// this. The brush models beside the levels - the ammo boxes, the health packs, the exploding
// boxes - are a few KB each.
constexpr int kMinLevelBytes = 32 * 1024;

enum class State
{
    Idle,     // Nothing issued yet; kick off the next map.
    Loading,  // Command issued, waiting for the world to come up.
    Dwelling, // Map is up; stay in it so it actually renders.
    Touring   // Perf pass: map is up; looking around from each of its viewpoints.
};

// A map that never comes up is a failed test, not a reason to hang forever.
constexpr int kLoadTimeoutMs = 90 * 1000;

// The level has to be up for this many frames before it counts as loaded: it guards the window
// between Cbuf_AddText and the command running, where the previous map is still up.
constexpr int kFramesToConfirm = 2;

static cvar_t s_enabled = ps2::MakeCvar("ps2_testmaps", "0", CVAR_NONE);
static cvar_t s_dwell   = ps2::MakeCvar("ps2_testmaps_dwell", "8", CVAR_NONE);
static cvar_t s_views   = ps2::MakeCvar("ps2_testmaps_views", "5", CVAR_NONE);

static MapEntry s_maps[kMaxMaps] = {}; // the levels to visit, in order (BuildMapList)
static int      s_mapCount       = 0;
static bool     s_listBuilt      = false;

static State  s_state         = State::Idle;
static int    s_nextMap       = 0;
static bool   s_done          = false;
static int    s_issuedAtMs    = 0;
static int    s_dwellUntilMs  = 0;
static int    s_confirmFrames = 0;
static int    s_failed        = 0;
static size_t s_peakBeforeMap = 0;
static int    s_hunkPeak      = 0; // Hunk in use plus cache, the most any map took.
static char   s_targetBsp[MAX_QPATH] = {};
static bool   s_perfPass      = false; // this pass is "ps2_testmaps 2"
static int    s_cutShort      = 0;     // perf pass: levels left before their tour ended

void Restart()
{
    s_state         = State::Idle;
    s_nextMap       = 0;
    s_done          = false;
    s_issuedAtMs    = 0;
    s_dwellUntilMs  = 0;
    s_confirmFrames = 0;
    s_failed        = 0;
    s_peakBeforeMap = 0;
    s_hunkPeak      = 0;
    s_targetBsp[0]  = '\0';
    s_cutShort      = 0;
    s_mapCount      = 0;
    s_listBuilt     = false; // rescanned, in case the game directory changed in between
}

bool TargetLevelIsUp()
{
    return cls.signon == SIGNONS && cl.worldmodel != nullptr && std::strcmp(cl.worldmodel->name, s_targetBsp) == 0;
}

// ------------------------------------------------------------------------------------------------
// The map list
// ------------------------------------------------------------------------------------------------

int CampaignIndex(const char * const name)
{
    for (int i = 0; i < ps2::ArrayLength(kCampaign); ++i)
    {
        if (std::strcmp(kCampaign[i], name) == 0)
        {
            return i;
        }
    }
    return ps2::ArrayLength(kCampaign);
}

bool InMapList(const char * const name)
{
    for (int i = 0; i < s_mapCount; ++i)
    {
        if (std::strcmp(s_maps[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

// Collects every level in the game data's paks: the campaign's first, in play order, then the rest
// alphabetically. Only paks are read, which is where id's maps live; a loose maps/ directory (a
// custom level dropped in) isn't looked at. Whatever the data lacks - the shareware pak has 9 of
// the campaign's 32 maps - is simply not in the list, and the start of the run says which.
void BuildMapList()
{
    s_mapCount  = 0;
    s_listBuilt = true;

    int tooLong = 0;
    int noRoom  = 0;

    // Highest priority first, so a level two paks both have is listed from the one the engine
    // would load it from.
    for (const searchpath_t * search = com_searchpaths; search != nullptr; search = search->next)
    {
        const pack_t * const pak = search->pack;
        if (pak == nullptr)
        {
            continue; // a directory
        }

        char pakName[sizeof(MapEntry::pak)];
        COM_StripExtension(COM_SkipPath(pak->filename), pakName, sizeof(pakName));

        for (int i = 0; i < pak->numfiles; ++i)
        {
            const packfile_t & file = pak->files[i];
            if (std::strncmp(file.name, "maps/", 5) != 0 || q_strcasecmp(COM_FileGetExtension(file.name), "bsp") != 0 ||
                file.filelen <= kMinLevelBytes)
            {
                continue;
            }

            char name[MAX_QPATH];
            COM_StripExtension(file.name + 5, name, sizeof(name));
            if (std::strlen(name) >= sizeof(MapEntry::name))
            {
                ++tooLong;
                continue;
            }
            if (InMapList(name))
            {
                continue;
            }
            if (s_mapCount == kMaxMaps)
            {
                ++noRoom;
                continue;
            }

            MapEntry & entry = s_maps[s_mapCount++];
            q_strlcpy(entry.name, name, sizeof(entry.name));
            q_strlcpy(entry.pak, pakName, sizeof(entry.pak));
        }
    }

    std::sort(s_maps, s_maps + s_mapCount, [](const MapEntry & a, const MapEntry & b) {
        const int ia = CampaignIndex(a.name);
        const int ib = CampaignIndex(b.name);
        return (ia != ib) ? (ia < ib) : (std::strcmp(a.name, b.name) < 0);
    });

    int campaign = 0;
    while (campaign < s_mapCount && CampaignIndex(s_maps[campaign].name) < ps2::ArrayLength(kCampaign))
    {
        ++campaign;
    }

    Con_Printf("MapCycle: %d maps - %d of the campaign's %d, then %d more.\n", s_mapCount, campaign,
               ps2::ArrayLength(kCampaign), s_mapCount - campaign);

    // The campaign maps the data doesn't have, on one line: a shareware run lists episodes 2-4
    // and end here, and that is all it says about them.
    if (campaign < ps2::ArrayLength(kCampaign))
    {
        char missing[256] = {};
        for (const char * const name : kCampaign)
        {
            if (!InMapList(name))
            {
                q_strlcat(missing, " ", sizeof(missing));
                q_strlcat(missing, name, sizeof(missing));
            }
        }
        Con_Printf("MapCycle: not in this game data:%s\n", missing);
    }
    if (tooLong > 0 || noRoom > 0)
    {
        Con_Printf("MapCycle: left out %d maps with names of %d characters or more, and %d past the first %d.\n",
                   tooLong, static_cast<int>(sizeof(MapEntry::name)), noRoom, kMaxMaps);
    }
}

// ------------------------------------------------------------------------------------------------
// The perf pass's tour
// ------------------------------------------------------------------------------------------------

// A spot the perf pass looks around from: the player's origin there (the eye is the view height
// above it), the pitch it looks at, and the yaw its turn starts from.
struct Viewpoint
{
    vec3_t       origin;
    float        pitch;
    float        yaw;
    const char * kind; // what marks it in the map: "spawn", "intermission", "deathmatch", "teleport"
};

// The spots a level can offer. id's offer 10 (dm1) to 28 (e4m6).
constexpr int kMaxCandidates = 64;

// The most viewpoints "ps2_testmaps_views" can ask of a level.
constexpr int kMaxViews = 16;

// A full turn at each viewpoint: 2 degrees a frame, 3 seconds at 60 fps. A step per frame rather
// than per second, so every run looks the same way in the same frame however fast it ran.
constexpr float kYawStep    = 2.0f;
constexpr int   kTurnFrames = 180;

// setpos travels to the server as a command, and the player's new origin comes back with the
// next update. A spot inside a trigger_teleport is never reached (the trigger moves the player
// on), so after this many frames the turn starts from wherever the player is.
constexpr int   kArriveFrames = 30;
constexpr float kArriveDist2  = 4.0f; // within 2 units: the protocol sends origins in 1/8ths

// QuakeC's info_teleport_destination raises itself 27 units at spawn, and a teleporter puts the
// player at its origin.
constexpr float kTeleportRaise = 27.0f;

static Viewpoint s_tour[kMaxViews] = {};
static int       s_tourCount = 0;
static int       s_tourView  = 0;  // the viewpoint being visited
static int       s_tourFrame = 0;  // frames spent at it, arriving included
static int       s_turnFrame = -1; // frames into its turn; -1 while still arriving
static int       s_lastFrame = 0;  // host_framecount when the tour last advanced

// Reads the spots a tour can use out of the level's entity lump, the map's own text: the
// intermission cameras, the deathmatch starts and the teleporters' exits. All three are spread
// over the level, and the cameras frame the views its designer wanted seen. The coop starts are
// left out: they crowd the single-player one.
int CollectCandidates(Viewpoint * const out)
{
    int count = 0;
    const char * data = cl.worldmodel->entities;
    while (count < kMaxCandidates)
    {
        data = COM_Parse(data);
        if (data == nullptr || com_token[0] != '{')
        {
            break;
        }

        char   classname[64] = {};
        vec3_t origin = { 0.0f, 0.0f, 0.0f };
        vec3_t mangle = { 0.0f, 0.0f, 0.0f };
        float  angle  = 0.0f;
        for (;;)
        {
            data = COM_Parse(data);
            if (data == nullptr || com_token[0] == '}')
            {
                break;
            }

            char key[64];
            q_strlcpy(key, com_token, sizeof(key));
            data = COM_ParseEx(data, CPE_ALLOWTRUNC);
            if (data == nullptr)
            {
                break;
            }

            if (std::strcmp(key, "classname") == 0)
            {
                q_strlcpy(classname, com_token, sizeof(classname));
            }
            else if (std::strcmp(key, "origin") == 0)
            {
                std::sscanf(com_token, "%f %f %f", &origin[0], &origin[1], &origin[2]);
            }
            else if (std::strcmp(key, "mangle") == 0)
            {
                std::sscanf(com_token, "%f %f %f", &mangle[0], &mangle[1], &mangle[2]);
            }
            else if (std::strcmp(key, "angle") == 0)
            {
                angle = static_cast<float>(std::atof(com_token));
            }
        }
        if (data == nullptr)
        {
            break; // a truncated lump: keep what came before it
        }

        Viewpoint v = {};
        VectorCopy(origin, v.origin);
        v.yaw = angle;
        if (std::strcmp(classname, "info_intermission") == 0)
        {
            // The camera is the eye: the intermission takes the view height away.
            v.origin[2] -= static_cast<float>(DEFAULT_VIEWHEIGHT);
            v.pitch = mangle[0];
            v.yaw   = mangle[1];
            v.kind  = "intermission";
        }
        else if (std::strcmp(classname, "info_player_deathmatch") == 0)
        {
            v.kind = "deathmatch";
        }
        else if (std::strcmp(classname, "info_teleport_destination") == 0)
        {
            v.origin[2] += kTeleportRaise;
            v.kind = "teleport";
        }
        else
        {
            continue;
        }
        out[count++] = v;
    }
    return count;
}

// Picks the tour: where the player spawned, then each time the candidate farthest from every
// spot already picked, so the views spread over the level rather than bunching where most of its
// teleporters are. Ties go to the earlier entity, so a level always gets the same tour.
void PlanTour()
{
    Viewpoint candidates[kMaxCandidates];
    const int numCandidates = CollectCandidates(candidates);

    Viewpoint & spawn = s_tour[0];
    VectorCopy(cl_entities[cl.viewentity].origin, spawn.origin);
    spawn.pitch = 0.0f;
    spawn.yaw   = cl.viewangles[YAW];
    spawn.kind  = "spawn";
    s_tourCount = 1;

    const int asked  = static_cast<int>(s_views.value);
    const int wanted = (asked < 1) ? 1 : (asked > kMaxViews) ? kMaxViews : asked;

    bool taken[kMaxCandidates] = {};
    while (s_tourCount < wanted)
    {
        int   best     = -1;
        float bestDist = -1.0f;
        for (int c = 0; c < numCandidates; ++c)
        {
            if (taken[c])
            {
                continue;
            }
            float nearest = 1.0e30f;
            for (int t = 0; t < s_tourCount; ++t)
            {
                vec3_t d;
                VectorSubtract(candidates[c].origin, s_tour[t].origin, d);
                const float dist = DotProduct(d, d);
                nearest = (dist < nearest) ? dist : nearest;
            }
            if (nearest > bestDist)
            {
                bestDist = nearest;
                best     = c;
            }
        }
        if (best < 0)
        {
            break; // fewer spots than asked for
        }
        taken[best] = true;
        s_tour[s_tourCount++] = candidates[best];
    }
}

void StartTour(const MapEntry & map, const int index)
{
    // Nothing in the level may cut the tour short or make two runs differ: god mode keeps the
    // player alive, and with notarget the monsters stay where the level put them.
    Cbuf_AddText("god 1\nnotarget 1\n");

    PlanTour();
    s_tourView  = 0;
    s_tourFrame = 0;
    s_turnFrame = -1;
    s_lastFrame = host_framecount;

    char kinds[kMaxViews * 14] = {};
    for (int i = 0; i < s_tourCount; ++i)
    {
        q_strlcat(kinds, (i > 0) ? ", " : "", sizeof(kinds));
        q_strlcat(kinds, s_tour[i].kind, sizeof(kinds));
    }
    Con_Printf("MapCycle [%2d/%2d] %-6s %-5s %d viewpoints: %s\n", index + 1, s_mapCount, map.name, map.pak,
               s_tourCount, kinds);
}

// Runs one frame of the tour. Returns false once every viewpoint has had its turn.
bool AdvanceTour()
{
    if (s_tourView >= s_tourCount)
    {
        return false;
    }

    // The main loop calls the test on every pass, and Host_Frame only runs a frame on some of them
    // (Host_FilterTime holds it to host_maxfps): the tour counts the frames that ran.
    if (host_framecount == s_lastFrame)
    {
        return true;
    }
    s_lastFrame = host_framecount;
    const Viewpoint & v = s_tour[s_tourView];

    if (s_tourFrame == 0)
    {
        // The marker goes first: the rows after it are this viewpoint's, arriving included.
        char what[96];
        std::snprintf(what, sizeof(what), "%d,%s,%.0f %.0f %.0f", s_tourView + 1, v.kind,
                      static_cast<double>(v.origin[0]), static_cast<double>(v.origin[1]),
                      static_cast<double>(v.origin[2]));
        ps2::debug::FrameLogMarkView(what);

        if (s_tourView > 0)
        {
            Cbuf_AddText(va("setpos %.1f %.1f %.1f\n", static_cast<double>(v.origin[0]),
                            static_cast<double>(v.origin[1]), static_cast<double>(v.origin[2])));
        }
    }

    if (s_turnFrame < 0)
    {
        // The spawn is where the player already stands.
        vec3_t d;
        VectorSubtract(cl_entities[cl.viewentity].origin, v.origin, d);
        const bool there = (s_tourView == 0) || DotProduct(d, d) < kArriveDist2;
        if (there || s_tourFrame >= kArriveFrames)
        {
            if (!there)
            {
                Con_Printf("MapCycle: viewpoint %d (%s) was never reached - turning where the player is.\n",
                           s_tourView + 1, v.kind);
            }
            s_turnFrame = 0;
        }
    }

    // Facing the way the turn starts while arriving, then around in steps.
    const float step = static_cast<float>((s_turnFrame > 0) ? s_turnFrame : 0);
    cl.viewangles[PITCH] = v.pitch;
    cl.viewangles[YAW]   = anglemod(v.yaw + kYawStep * step);
    cl.viewangles[ROLL]  = 0.0f;

    ++s_tourFrame;
    if (s_turnFrame >= 0 && ++s_turnFrame >= kTurnFrames)
    {
        ++s_tourView;
        s_tourFrame = 0;
        s_turnFrame = -1;
    }
    return true;
}

// Marks where the level's tour stops counting. The next map command runs in a frame that still
// draws this level, and its load lands in that row; the marker keeps the row out of the level's
// figures.
void EndTour()
{
    ps2::debug::FrameLogMarkView("0,end,-");
}

// ------------------------------------------------------------------------------------------------
// Reports
// ------------------------------------------------------------------------------------------------

size_t TagBytes(const ps2::heap::MemTag tag)
{
    return ps2::heap::GetStatsForMemTag(tag).totalBytes;
}

// One line per map: QuakeSpasm's hunk (the level, and the cache of models and sounds it loaded,
// and what is left of the hunk after both), then the backend's level data by tag and the
// program's totals. "NEW PEAK" marks the map whose load took the most of the heap.
void ReportMap(const MapEntry & map, const int index)
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

    Con_Printf("MapCycle [%2d/%2d] %-6s %-5s Hunk %-9s Cache %-9s HunkLeft %-9s | World %-9s Light %-9s Tex %-9s "
               "Mdl %-9s Mus %-9s | PEAK %-9s FREE %-9s%s\n",
               index + 1, s_mapCount, map.name, map.pak,
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

    Con_Printf("MapCycle [%2d/%2d] %-6s %-5s load peak %-9s ARENA %-9s\n",
               index + 1, s_mapCount, map.name, map.pak,
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

    Con_Printf("MapCycle: pass complete - %d of %d maps loaded, %d timed out.\n",
               s_mapCount - s_failed, s_mapCount, s_failed);
    if (s_cutShort > 0)
    {
        Con_Printf("MapCycle: %d levels were left before their tour ended.\n", s_cutShort);
    }
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

#if PS2_QUAKE_PROFILE
    if (s_perfPass)
    {
        EndPerfCapture();
    }
#endif // PS2_QUAKE_PROFILE
}

// Reads which pass this is and lists the maps. The perf pass starts its capture here, before the
// first map loads, so the frame log marks that map too.
void BeginPass()
{
    s_perfPass = (static_cast<int>(s_enabled.value) == 2);

#if PS2_QUAKE_PROFILE
    // Once per boot: a "ps2_testmaps_restart" mid-pass starts the maps over in the same capture.
    static bool s_captureStarted = false;
    if (s_perfPass && !s_captureStarted)
    {
        s_captureStarted = true;
        BeginPerfCapture();
    }
#else
    if (s_perfPass)
    {
        Con_Printf("MapCycle: the perf pass needs a profile build (PS2_QUAKE_PROFILE) - running the memory pass.\n");
        s_perfPass = false;
    }
#endif // PS2_QUAKE_PROFILE

    BuildMapList();

    if (s_perfPass)
    {
        Con_Printf("MapCycle: perf pass - up to %d viewpoints a level, a full turn at each; developer 0, overlays "
                   "off, frame log and file-open notes on.\n",
                   static_cast<int>(s_views.value));
    }
}

// Issues the next map of the list. Returns false when the list is exhausted.
bool StartNextMap()
{
    if (s_nextMap >= s_mapCount)
    {
        return false;
    }

    const char * const name = s_maps[s_nextMap].name;
    std::snprintf(s_targetBsp, sizeof(s_targetBsp), "maps/%s.bsp", name);

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

} // namespace

void InitMapCycle()
{
    Cvar_RegisterVariable(&s_enabled);
    Cvar_RegisterVariable(&s_dwell);
    Cvar_RegisterVariable(&s_views);
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
        if (!s_listBuilt)
        {
            BeginPass();
        }
        if (!StartNextMap())
        {
            Finish();
        }
        break;

    case State::Loading:
        if (TargetLevelIsUp() && ++s_confirmFrames >= kFramesToConfirm)
        {
            if (s_perfPass)
            {
                StartTour(s_maps[s_nextMap], s_nextMap);
                s_state = State::Touring;
                break;
            }
            const int dwellMs = static_cast<int>(s_dwell.value * 1000.0f);
            s_dwellUntilMs = ps2::sys::Milliseconds() + ((dwellMs > 0) ? dwellMs : 1);
            s_state = State::Dwelling;
            break;
        }
        if ((ps2::sys::Milliseconds() - s_issuedAtMs) > kLoadTimeoutMs)
        {
            Con_Printf("MapCycle: '%s' never came up after %d seconds - moving on.\n",
                       s_maps[s_nextMap].name, kLoadTimeoutMs / 1000);
            ++s_failed;
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;

    case State::Dwelling:
        if (ps2::sys::Milliseconds() >= s_dwellUntilMs)
        {
            ReportMap(s_maps[s_nextMap], s_nextMap);
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;

    case State::Touring:
        if (!TargetLevelIsUp())
        {
            // Noclip still touches triggers, so a viewpoint inside a trigger_changelevel ends the
            // level. The frame log names whatever loaded instead with its own map marker.
            Con_Printf("MapCycle: left '%s' at viewpoint %d of %d - moving on.\n", s_maps[s_nextMap].name,
                       s_tourView + 1, s_tourCount);
            EndTour();
            ++s_cutShort;
            ++s_nextMap;
            s_state = State::Idle;
            break;
        }
        if (!AdvanceTour())
        {
            EndTour();
            ReportMap(s_maps[s_nextMap], s_nextMap);
            ++s_nextMap;
            s_state = State::Idle;
        }
        break;
    }
}

} // namespace ps2::test
#endif // PS2_QUAKE_DEBUG

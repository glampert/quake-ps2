/* ================================================================================================
 * File: loading_screen.cpp
 * Brief: When the loading screen is drawn, and what its status bar says. See loading_screen.h.
 *        The drawing itself is DrawLoadingScreen, in vid.cpp with the frame bracket.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/engine_hooks.h"
#include "ps2/renderer/loading_screen.h"
#include "ps2/debug/io_log.h"
#include "ps2/debug/load_trace.h"
#include "ps2/system/sys.h"

#include <cstdio>
#include <tamtypes.h>

namespace ps2::loading {
namespace {

// Each loading screen frame waits for a vsync to be shown. Redrawing at most this often keeps
// that to a few percent of a load made of small files.
constexpr int kRedrawIntervalMsec = 200;

// A file this large takes a while to read from a USB drive (roughly 1 MB/s), so the bar names it
// as soon as it starts, however recently the screen was redrawn.
constexpr int kLargeFileBytes = 256 * 1024;

static bool s_active       = false;
static int  s_beginMsec    = 0;
static int  s_lastDrawMsec = 0;
static int  s_drawCount    = 0;
static int  s_fileCount    = 0;
static u32  s_bytesRead    = 0;
static char s_fileName[MAX_QPATH] = "?";

void Draw()
{
    const int msec = ps2::sys::Milliseconds();
    const u32 tenthsOfMb = s_bytesRead / ((1024u * 1024u) / 10u);

    char status[MAX_QPATH + 64];
    std::snprintf(status, sizeof(status), "%s  [%d files, %u.%u MB, %d s]",
                  (s_fileCount > 0) ? s_fileName : "Loading...", s_fileCount, tenthsOfMb / 10u,
                  tenthsOfMb % 10u, (msec - s_beginMsec) / 1000);

    // A frame that couldn't be drawn doesn't count, so the next read redraws at once.
    if (DrawLoadingScreen(status))
    {
        s_lastDrawMsec = ps2::sys::Milliseconds();
        s_drawCount += 1;
    }
}

} // namespace

void NoteFile(const char * fileName)
{
    std::snprintf(s_fileName, sizeof(s_fileName), "%s", fileName);
    if (s_active)
    {
        s_fileCount += 1;
    }
}

void NoteRead(const int bytes)
{
    if (!s_active)
    {
        return;
    }

    s_bytesRead += (bytes > 0) ? static_cast<u32>(bytes) : 0u;

    if ((ps2::sys::Milliseconds() - s_lastDrawMsec) >= kRedrawIntervalMsec || bytes >= kLargeFileBytes)
    {
        Draw();
    }
}

bool Shown()
{
    return s_active;
}

const char * CurrentFile()
{
    return s_fileName;
}

bool Redraw()
{
    if (!s_active)
    {
        return false;
    }
    Draw();
    return true;
}

} // namespace ps2::loading

// ------------------------------------------------------------------------------------------------
// engine_hooks.h
// ------------------------------------------------------------------------------------------------

void PS2_LoadingScreenBegin()
{
    using namespace ps2::loading;

    // The I/O log, when built in, records the plaque's lifetime (debug/io_log.h).
    ps2::iolog::LoadBegin();

    s_active    = true;
    s_beginMsec = ps2::sys::Milliseconds();
    s_drawCount = 0;
    s_fileCount = 0;
    s_bytesRead = 0;

    PS2_LOAD_TRACE("Loading plaque up");
    Draw();
}

void PS2_LoadingScreenEnd()
{
    using namespace ps2::loading;

    if (s_active)
    {
        PS2_LOAD_TRACE("Loading plaque down: %d frames drawn over %d files and %u KB, in %d ms",
                       s_drawCount, s_fileCount, s_bytesRead / 1024u, ps2::sys::Milliseconds() - s_beginMsec);
    }
    s_active = false;
    ps2::iolog::LoadEnd();
}

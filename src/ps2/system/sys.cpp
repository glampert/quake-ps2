/* ================================================================================================
 * File: sys.cpp
 * Brief: QuakeSpasm's Sys_* platform seam for the PS2 - timing, fatal errors, console output and
 *        file handles - plus the PL_* stubs and the backend's clock (sys.h). There is no console
 *        input on the target, and no window, so those parts are stubbed.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/sys.h"
#include "ps2/system/iop_boot.h"
#include "ps2/debug/scr_print.h"
#include "ps2/debug/profile.h"
#include "ps2/renderer/profile.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>      // nanosleep
#include <sys/stat.h> // stat, mkdir

#include <kernel.h> // SleepThread
#include <timer.h>  // GetTimerSystemTime / kBUSCLK / kBUSCLKBY256

// ------------------------------------------------------------------------------------------------
// The backend's clock
// ------------------------------------------------------------------------------------------------

namespace ps2::sys {

// Deliberately not clock(): the R5900 has no DMULT/DDIV, so every 64-bit
// multiply or divide - even by a constant - becomes a libgcc __muldi3 /
// __udivdi3 call. clock() pays two of those inside TimerBusClock2USec before
// we get a number, then the microseconds-to-milliseconds conversion pays a
// third. Reading the system timer directly and converting in 32-bit costs none.
int Milliseconds()
{
    // The EE system timer (T2) is clocked at BUSCLK/256, and GetTimerSystemTime
    // scales its count back up into BUSCLK units, so the low 8 bits are always
    // zero. Shifting them off recovers the raw 576000Hz tick - exactly 576 per
    // millisecond - which is also the timer's true 1.736us resolution.
    constexpr u32 kTicksPerMillisec = kBUSCLKBY256 / 1000; // 576

    static u64  s_lastBusClk    = 0;
    static u32  s_tickRemainder = 0;
    static int  s_millisecs     = 0;
    static bool s_initialized   = false;

    const u64 nowBusClk = GetTimerSystemTime();
    if (!s_initialized)
    {
        s_lastBusClk  = nowBusClk;
        s_initialized = true;
    }

    // Only the delta stays 64-bit (a single dsubu) and it narrows safely: 32
    // bits of 576kHz ticks is over two hours between calls.
    const u32 deltaTicks = static_cast<u32>((nowBusClk - s_lastBusClk) >> 8);
    s_lastBusClk = nowBusClk;

    // Carry the sub-millisecond remainder so truncation doesn't lose time.
    s_tickRemainder += deltaTicks;
    s_millisecs     += static_cast<int>(s_tickRemainder / kTicksPerMillisec);
    s_tickRemainder %= kTicksPerMillisec;

    return s_millisecs;
}

// TODO: Make this single-precision float.
double Seconds()
{
    // A soft-float conversion and multiply per call, which is fine for what calls it -
    // QuakeSpasm reads the time a handful of times per frame. The 64-bit BUSCLK count
    // wraps after nearly four thousand years.
    constexpr double kSecondsPerBusClk = 1.0 / static_cast<double>(kBUSCLK);

    static u64  s_startBusClk = 0;
    static bool s_initialized = false;

    const u64 nowBusClk = GetTimerSystemTime();
    if (!s_initialized)
    {
        s_startBusClk = nowBusClk;
        s_initialized = true;
    }

    return static_cast<double>(nowBusClk - s_startBusClk) * kSecondsPerBusClk;
}

void RegisterCommands()
{
    Cmd_AddCommand("ps2_dump_iop_mods", []() {
        PrintLoadedIopModules(40, &Con_Printf);
    });

    // The program's memory as the backend sees it. QuakeSpasm's hunk is a single Hunk-tagged
    // block, so how full it is is hunk_print's to say; what QuakeSpasm takes with plain malloc
    // (sv.edicts, mostly) carries no tag and shows up as the untagged remainder.
    Cmd_AddCommand("ps2_memstats", []() {
        char dump[ps2::heap::kMemTagsDumpSize];
        Con_Printf("%s\n", ps2::heap::DumpMemTags(dump, sizeof(dump)));

        size_t taggedHeapBytes = 0;
        for (int i = 0; i < static_cast<int>(ps2::heap::MemTag::TagCount); ++i)
        {
            const auto tag = static_cast<ps2::heap::MemTag>(i);
            if (tag != ps2::heap::MemTag::ElfSys) // the system's RAM, not the heap's
            {
                taggedHeapBytes += ps2::heap::GetStatsForMemTag(tag).totalBytes;
            }
        }

        const ps2::heap::HeapStats heap = ps2::heap::GetHeapStats();
        const size_t untaggedBytes = (heap.inUseBytes > taggedHeapBytes) ? (heap.inUseBytes - taggedHeapBytes) : 0u;

        char arena[ps2::heap::kMemUnitStrSize];
        char inUse[ps2::heap::kMemUnitStrSize];
        char untagged[ps2::heap::kMemUnitStrSize];
        char freeBytes[ps2::heap::kMemUnitStrSize];
        Con_Printf("dlmalloc: arena %s, in use %s (untagged malloc %s), free %s in %u chunks\n",
                   ps2::heap::FormatMemoryUnit(heap.arenaBytes, true, arena, sizeof(arena)),
                   ps2::heap::FormatMemoryUnit(heap.inUseBytes, true, inUse, sizeof(inUse)),
                   ps2::heap::FormatMemoryUnit(untaggedBytes, true, untagged, sizeof(untagged)),
                   ps2::heap::FormatMemoryUnit(heap.freeBytes, true, freeBytes, sizeof(freeBytes)),
                   static_cast<unsigned>(heap.freeChunks));
    });

#if PS2_QUAKE_PROFILE
    Cmd_AddCommand("ps2_profile", []() {
        ps2::debug::ProfileDump(&Con_Printf);
    });

    Cmd_AddCommand("ps2_profile_reset", []() {
        ps2::debug::ProfileReset();
    });
#endif // PS2_QUAKE_PROFILE
}

} // namespace ps2::sys

namespace {

// ------------------------------------------------------------------------------------------------
// Console output
// ------------------------------------------------------------------------------------------------

// The engine's text, prefixed for the log, in as few writes as it takes: stdout reaches the
// PCSX2 log (or ps2client) through the IOP, one SIF RPC per write.
class LogWriter final
{
public:
    void Put(const char c)
    {
        if (m_length == static_cast<int>(sizeof(m_buffer)))
        {
            Flush();
        }
        m_buffer[m_length++] = c;
    }

    void Flush()
    {
        if (m_length > 0)
        {
            std::fwrite(m_buffer, 1, static_cast<size_t>(m_length), stdout);
            m_length = 0;
        }
    }

private:
    char m_buffer[512];
    int  m_length = 0;
};

// Quake's console font has glyphs below 32 - and again, coloured, above 127 - that a log can't
// show. This maps them to the nearest ASCII: the separator bar to '-', the bracket and digit
// glyphs to themselves, the rest to '.'. A leading 1 or 2 only marks a line as coloured, so those
// two are dropped ('\0').
char LogChar(const unsigned char glyph)
{
    const unsigned char c = glyph & 0x7Fu;
    if (c >= ' ' || c == '\n' || c == '\t')
    {
        return static_cast<char>(c);
    }
    if (c == 1 || c == 2)
    {
        return '\0';
    }
    if (c == 0x10 || c == 0x11)
    {
        return (c == 0x10) ? '[' : ']';
    }
    if (c >= 0x12 && c <= 0x1B)
    {
        return static_cast<char>('0' + (c - 0x12));
    }
    return (c >= 0x1D) ? '-' : '.'; // 0x1D-0x1F are the bar's left end, middle and right end
}

// Writes 'text' to stdout with "[Q1] " at the start of every line, so engine output can be told
// from the emulator's in the PCSX2 log. QuakeSpasm often prints a line in several pieces, so the
// prefix goes where a line starts, not once per call.
void PrintToLog(const char * text)
{
    static bool s_atLineStart = true;

    LogWriter writer;
    for (const char * s = text; *s != '\0'; ++s)
    {
        if (s_atLineStart)
        {
            for (const char * prefix = "[Q1] "; *prefix != '\0'; ++prefix)
            {
                writer.Put(*prefix);
            }
            s_atLineStart = false;
        }

        const char c = LogChar(static_cast<unsigned char>(*s));
        if (c == '\0')
        {
            continue;
        }
        if (c == '\n')
        {
            s_atLineStart = true;
        }
        writer.Put(c);
    }

    writer.Flush();
    if (s_atLineStart)
    {
        std::fflush(stdout);
    }
}

// ------------------------------------------------------------------------------------------------
// File handles
// ------------------------------------------------------------------------------------------------

// QuakeSpasm's handle-based file API over stdio, as its Unix backend did it. Slot 0 is never
// handed out, so a zero handle can't be mistaken for an open file.
constexpr int kMaxFileHandles = 32;
static std::FILE * s_fileHandles[kMaxFileHandles];

int FindFileHandle()
{
    for (int i = 1; i < kMaxFileHandles; ++i)
    {
        if (s_fileHandles[i] == nullptr)
        {
            return i;
        }
    }
    Sys_Error("out of file handles");
}

long FileLength(std::FILE * file)
{
    const long pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    const long end = std::ftell(file);
    std::fseek(file, pos, SEEK_SET);
    return end;
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// Globals QuakeSpasm expects the platform layer to own
// ------------------------------------------------------------------------------------------------

qboolean isDedicated = false; // set from -dedicated by main()

// Registered by host.c. QuakeSpasm's desktop main loop sleeps while a frame is shorter than
// this; the PS2 one never sleeps (the vsync wait paces it), but configs still set it.
cvar_t sys_throttle = ps2::MakeCvar("sys_throttle", "0.02", CVAR_ARCHIVE);

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void Sys_Init()
{
    // main() set basedir to wherever the game data was found (host: or mass:). There is
    // no separate user directory on the PS2, and code elsewhere relies on userdir being
    // the same pointer as basedir when that is so.
    host_parms->userdir = host_parms->basedir;
    host_parms->numcpus = 1;

#if PS2_QUAKE_PROFILE
    // Learn the real COP0 Count rate before any probe can fire (~8ms spin).
    ps2::debug::ProfileCalibrate();
#endif // PS2_QUAKE_PROFILE
}

void Sys_Quit()
{
    Host_Shutdown();
    std::fflush(stdout);
    std::exit(0);
}

void Sys_Error(const char * error, ...)
{
    char text[1024];

    va_list argptr;
    va_start(argptr, error);
    std::vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    // Also to the log, so a capture says why the run stopped. No Host_Shutdown, unlike
    // QuakeSpasm's desktop Sys_Error: it would write config.cfg out of whatever state the
    // error left the engine in.
    PrintToLog("\nSys_Error: ");
    PrintToLog(text);
    PrintToLog("\n");

    ps2::debug::ScrInit();
    ps2::debug::ScrSetTextColor(0xFF0000FF); // red text
    ps2::debug::ScrPrintf("***************************************************************\n");
    ps2::debug::ScrPrintf("Sys_Error:\n%s\n", text);
    ps2::debug::ScrPrintf("***************************************************************\n");

    // Draw the error to the screen and halt so the
    // message stays readable in the emulator/console.
    for (;;)
    {
        SleepThread();
    }
}

void Sys_Printf(const char * fmt, ...)
{
    char text[2048];

    va_list argptr;
    va_start(argptr, fmt);
    std::vsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);

    PrintToLog(text);
}

double Sys_DoubleTime()
{
    return ps2::sys::Seconds();
}

const char * Sys_ConsoleInput()
{
    return nullptr; // no terminal to type into on the PS2
}

void Sys_Sleep(unsigned long msecs)
{
    timespec request;
    request.tv_sec  = static_cast<time_t>(msecs / 1000u);
    request.tv_nsec = static_cast<long>((msecs % 1000u) * 1000000u);
    nanosleep(&request, nullptr);
}

void Sys_SendKeyEvents()
{
    IN_Commands();
    IN_SendKeyEvents();
}

// ------------------------------------------------------------------------------------------------
// File IO
// ------------------------------------------------------------------------------------------------

// The opens, seeks and reads below are what COM_LoadFile and the pak reads come down to, so they
// carry the FsIo profile event: every load the engine makes is timed, whichever phase made it.

int Sys_FileOpenRead(const char * path, int * hndl)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::FsIo);

    const int handle = FindFileHandle();
    std::FILE * file = std::fopen(path, "rb");

    if (file == nullptr)
    {
        *hndl = -1;
        return -1;
    }

    s_fileHandles[handle] = file;
    *hndl = handle;
    return static_cast<int>(FileLength(file));
}

int Sys_FileOpenWrite(const char * path)
{
    const int handle = FindFileHandle();
    std::FILE * file = std::fopen(path, "wb");

    if (file == nullptr)
    {
        Sys_Error("Error opening %s: %s", path, std::strerror(errno));
    }

    s_fileHandles[handle] = file;
    return handle;
}

void Sys_FileClose(int handle)
{
    std::fclose(s_fileHandles[handle]);
    s_fileHandles[handle] = nullptr;
}

void Sys_FileSeek(int handle, int position)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::FsIo);
    std::fseek(s_fileHandles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void * dest, int count)
{
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::FsIo);
    return static_cast<int>(std::fread(dest, 1, static_cast<size_t>(count), s_fileHandles[handle]));
}

int Sys_FileWrite(int handle, const void * data, int count)
{
    return static_cast<int>(std::fwrite(data, 1, static_cast<size_t>(count), s_fileHandles[handle]));
}

int Sys_FileType(const char * path)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return FS_ENT_NONE;
    }
    if (S_ISDIR(st.st_mode))
    {
        return FS_ENT_DIRECTORY;
    }
    if (S_ISREG(st.st_mode))
    {
        return FS_ENT_FILE;
    }
    return FS_ENT_NONE;
}

// COM_CreatePath calls this for every directory along a path, the device root ("host:")
// included, so failures - that one, and directories that already exist - are expected and
// ignored, where QuakeSpasm's desktop version stopped with a Sys_Error. newlib's mkdir reaches
// host: through the ROM FILEIO and mass: through fileXio.
void Sys_mkdir(const char * path)
{
    mkdir(path, 0777);
}

// ------------------------------------------------------------------------------------------------
// Platform stubs (platform.h): no window, no clipboard, no dialogs
// ------------------------------------------------------------------------------------------------

void PL_SetWindowIcon() {}
void PL_VID_Shutdown() {}
char * PL_GetClipboardData() { return nullptr; }
void PL_ErrorDialog(const char * text) { (void)text; } // Sys_Error prints to the screen itself

} // extern "C"

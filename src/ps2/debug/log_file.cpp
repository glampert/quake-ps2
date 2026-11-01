/* ================================================================================================
 * File: log_file.cpp
 * Brief: The console log on the game data's drive. See log_file.h.
 *
 *  Every write opens the file, appends and closes it again, because closing it is the only way
 *  to get a line onto a USB drive. FatFs keeps a file's last partial sector in a buffer of its
 *  own and writes the file's size into its directory entry only when the file is synced or
 *  closed. bdmfs_fatfs has no sync op (it returns EIO) and libcglue's fsync() is ENOSYS, so a
 *  log held open could come back empty after the console is switched off. BDM's block cache
 *  writes through, so once the file is closed the line is on the drive. That costs a few USB
 *  commands per write, which is fine for a log written mostly while loading.
 *  PFS keeps inode/directory metadata cached even after close with our read/write mount.
 *  After each close the storage boot layer syncs HDD metadata; on host:/USB it does nothing.
 *  This also covers the header and fatal paths.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/debug/log_file.h"
#include "ps2/debug/load_trace.h"
#include "ps2/system/iop_boot.h"
#include "ps2/system/sys.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <kernel.h> // CreateSema, WaitSema, SignalSema, GetThreadId

namespace ps2::debug {
namespace {

constexpr const char * kLogFileName = "quake.log";

// What the log does until ps2_logfile is registered, and that cvar's default: on in a build with
// the load trace on, which is read back from the log.
constexpr bool kLogFileDefaultOn = (PS2_QUAKE_LOAD_TRACE != 0);

// A Con_Printf message (MAXPRINTMSG is 4096) with a stamp on each line fits in one write. A
// longer run of short lines just takes more than one.
constexpr int kBufferSize = 8192;

// What is kept while the log is off, to go in ahead of its first line (see LogFileOpen): a boot
// and a level load fit. Past it, lines are dropped, and the log says so where they would be.
constexpr int kBacklogBytes = 16 * 1024;

enum class State
{
    Off,     // no path yet, or the file couldn't be written: nothing is logged
    Pending, // has a path; the first line written creates the file, emptying an old one
    Open,    // created, and every write since has gone in
};

static State s_state = State::Off;
static char  s_path[MAX_OSPATH];

// Not archived: a debugging aid shouldn't stay off across sessions because one run turned it off.
// It reads 0 until registered, so the default stands in until then.
static cvar_t s_logEnabled = ps2::MakeCvar("ps2_logfile", kLogFileDefaultOn ? "1" : "0", CVAR_NONE);
static bool   s_cvarRegistered = false;

// Serializes writers, for any thread that prints (the music stream's reader thread once did; the
// background loader never does). The owner is what lets a fatal path tell its own thread's
// interrupted write from another thread's.
static int s_lockSema  = -1;
static int s_lockOwner = -1;

// A fatal report is being written (LogFileWriteFatal): the file takes it, the backlog ahead of
// it, whether or not ps2_logfile is on. A console crash is the one report a run can't do without,
// and the Quake II port's 2026-10-27 out-of-memory screen left nothing behind because its log was
// off.
static bool s_forceOut = false;

static char s_buffer[kBufferSize];
static int  s_used = 0;
static bool s_atLineStart = true;

static char s_backlog[kBacklogBytes];
static int  s_backlogUsed    = 0;
static bool s_backlogDropped = false;

Q_ALWAYS_INLINE bool LogEnabled()
{
    return s_cvarRegistered ? (s_logEnabled.value != 0.0f) : kLogFileDefaultOn;
}

// Appends 'data' to the file, creating it on the first write. See the note at the top of the
// file for why every write is an open and a close.
void WriteOut(const char * data, const int size)
{
    if (size == 0 || s_state == State::Off)
    {
        return;
    }

    const int flags = (s_state == State::Pending) ? (O_WRONLY | O_CREAT | O_TRUNC) : O_WRONLY;
    const int fd = open(s_path, flags, 0666);
    bool written = false;
    int syncResult = 0;
    if (fd >= 0)
    {
        written = (lseek(fd, 0, SEEK_END) >= 0) && (write(fd, data, static_cast<size_t>(size)) == size);
        // Only a negative close is a failure. On USB and HDD the close goes through fileXio to
        // iomanX, whose close returns the descriptor's slot number on success, and libcglue
        // passes that through. Requiring 0 turned the Quake II port's log off after its first line.
        const int closeResult = close(fd);
        // Even a partial write or failed close may have changed HDD metadata.
        syncResult = ps2::sys::SyncGameDataDevice();
        written = written && closeResult >= 0 && syncResult == 0;
    }

    if (written)
    {
        s_state = State::Open;
        return;
    }

    // Give up after the first failure: a drive that refused one write would make every
    // Con_Printf after it wait on another.
    if (syncResult != 0)
    {
        std::printf("WARNING: can't sync the log file %s (PFS error %d) - logging is off.\n", s_path, syncResult);
    }
    else
    {
        std::printf("WARNING: can't write the log file %s (%d) - logging is off.\n", s_path, fd);
    }
    s_state = State::Off;
}

// Sends what the buffer holds on: to the file while the log is on (or a fatal report is being
// written), after whatever the backlog kept; to the backlog while it is off.
void Flush()
{
    if (LogEnabled() || s_forceOut)
    {
        if (s_backlogUsed > 0)
        {
            WriteOut(s_backlog, s_backlogUsed);
            if (s_backlogDropped)
            {
                static const char kDropped[] = "[log] (the backlog was full: lines were dropped up to here)\n";
                WriteOut(kDropped, static_cast<int>(sizeof(kDropped) - 1));
            }
            s_backlogUsed    = 0;
            s_backlogDropped = false;
        }
        WriteOut(s_buffer, s_used);
    }
    else if (s_backlogUsed + s_used <= kBacklogBytes)
    {
        std::memcpy(&s_backlog[s_backlogUsed], s_buffer, static_cast<size_t>(s_used));
        s_backlogUsed += s_used;
    }
    else
    {
        s_backlogDropped = true;
    }
    s_used = 0;
}

void Put(const char c)
{
    if (s_used == kBufferSize)
    {
        Flush();
    }
    s_buffer[s_used++] = c;
}

void PutStamp(const int msec)
{
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%4d.%03d] ", msec / 1000, msec % 1000);
    for (const char * c = stamp; *c != '\0'; ++c)
    {
        Put(*c);
    }
}

void Lock()
{
    if (s_lockSema >= 0)
    {
        WaitSema(s_lockSema);
        s_lockOwner = GetThreadId();
    }
}

void Unlock()
{
    if (s_lockSema >= 0)
    {
        s_lockOwner = -1;
        SignalSema(s_lockSema);
    }
}

// The caller holds the lock.
void WriteLocked(const char * text)
{
    // A full backlog takes nothing more until the log is on, or a fatal report comes. Blank text
    // goes in too, unlike in the Quake II port: QuakeSpasm ends a line it printed in pieces with a
    // lone "\n" (echo does).
    if (s_state == State::Off || (s_backlogDropped && !LogEnabled() && !s_forceOut))
    {
        return;
    }

    const int msec = ps2::sys::Milliseconds();
    for (; *text != '\0'; ++text)
    {
        if (s_atLineStart)
        {
            PutStamp(msec);
            s_atLineStart = false;
        }

        // A "\r" ends a progress line on the console, so it ends a line here too.
        const char c = (*text == '\r') ? '\n' : *text;
        Put(c);
        s_atLineStart = (c == '\n');
    }
    Flush();
}

} // namespace

void LogFileOpen(const char * basePath)
{
    std::snprintf(s_path, sizeof(s_path), "%s/%s", basePath, kLogFileName);

    ee_sema_t sema = {};
    sema.init_count = 1;
    sema.max_count  = 1;
    s_lockSema = CreateSema(&sema);

    s_state = State::Pending;

    char header[MAX_OSPATH + 96];
    std::snprintf(header, sizeof(header), "Quake (QuakeSpasm %s) for PS2 (%s build), game data on %s\n",
                  QUAKESPASM_VER_STRING, PS2_QUAKE_DEBUG ? "debug" : "release", basePath);
    LogFileWrite(header);

    if (s_state == State::Open)
    {
        std::printf("Logging to %s.\n", s_path);
    }
}

void LogFileRegisterCvar()
{
    Cvar_RegisterVariable(&s_logEnabled);
    s_cvarRegistered = true;
}

void LogFileWrite(const char * text)
{
    if (s_state == State::Off || text == nullptr)
    {
        return;
    }

    Lock();
    WriteLocked(text);
    Unlock();
}

void LogFilePrintf(const char * const format, ...)
{
    char text[640];

    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    LogFileWrite(text);
}

void LogFileWriteFatal(const char * text)
{
    if (s_state == State::Off || text == nullptr)
    {
        return;
    }

    // This thread's own write was cut short: it holds the lock already, and waiting for it
    // would never end. What that write left in the buffer goes out ahead of this.
    const bool ownWriteInterrupted = (s_lockSema >= 0) && (s_lockOwner == GetThreadId());
    if (!ownWriteInterrupted)
    {
        Lock();
    }

    // Not gated on ps2_logfile: with the log off, quake.log then holds the backlog (the boot report
    // and the last lines printed) and the report.
    s_forceOut = true;
    if (!s_atLineStart)
    {
        Put('\n');
        s_atLineStart = true;
    }
    WriteLocked(text);
    s_forceOut = false;

    if (!ownWriteInterrupted)
    {
        Unlock();
    }
}

void DumpPrintf(const char * const format, ...)
{
    // Static, like Sys_Error's log line: these run on whichever thread's stack failed.
    static char s_line[512];

    va_list args;
    va_start(args, format);
    std::vsnprintf(s_line, sizeof(s_line), format, args);
    va_end(args);

    std::fputs(s_line, stdout);
    std::fflush(stdout);
    LogFileWriteFatal(s_line);
}

} // namespace ps2::debug

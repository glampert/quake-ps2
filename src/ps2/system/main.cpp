/* ================================================================================================
 * File: main.cpp
 * Brief: PS2 application entry point. Finds the game data, hands QuakeSpasm its hunk, boots
 *        the host, then runs the frame loop forever. The PS2's counterpart of QuakeSpasm's
 *        main_sdl.c.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/iop_boot.h"
#include "ps2/renderer/profile.h"
#include "ps2/debug/exception_handler.h"

#include <cstdlib>

namespace {

// QuakeSpasm's hunk, one block that also holds its zone and its cache (see zone.c): nearly every
// level, model, sound and texture the engine loads lands in it. A starting size, until measured
// level loads give a real one; -heapsize <KB> overrides it, as on the desktop.
constexpr int kDefaultHunkSizeBytes = 16 * 1024 * 1024;

// Must outlive the program: host_parms points at it.
static quakeparms_t s_parms;

// The command line QuakeSpasm sees. Like any C program it takes argv[0] to be the program and
// looks for options from argv[1] on, but launchers disagree on argv[0]: PCSX2's -gameargs hands
// the ELF its arguments alone, so a lone "-dedicated" arrives as argv[0] and would be skipped.
// A first argument that looks like an option or a +command gets a program name put in front.
static char   s_programName[] = "quake.elf";
static char * s_argv[MAX_NUM_ARGVS + 1];
static int    s_argc = 0;

void BuildArgv(const int argc, char ** const argv)
{
    const bool argv0IsArg = (argc > 0) && (argv[0][0] == '-' || argv[0][0] == '+');
    if (argc == 0 || argv0IsArg)
    {
        s_argv[s_argc++] = s_programName;
    }
    for (int i = 0; i < argc && s_argc < MAX_NUM_ARGVS; ++i)
    {
        s_argv[s_argc++] = argv[i];
    }
    s_argv[s_argc] = nullptr;
}

int HunkSizeBytes()
{
    const int arg = COM_CheckParm("-heapsize");
    if (arg != 0 && arg < com_argc - 1)
    {
        return Q_atoi(com_argv[arg + 1]) * 1024;
    }
    return kDefaultHunkSizeBytes;
}

} // namespace

int main(int argc, char ** argv)
{
#if PS2_QUAKE_DEBUG
    // First thing, ahead of even the memory accounting: a bad pointer any time
    // after this prints its cause, the faulting instruction and a call stack
    // instead of hanging the EE with three lines of emulator output. Costs
    // nothing until something faults, and is compiled out of release.
    ps2::debug::InstallExceptionHandlers();
#endif // PS2_QUAKE_DEBUG

    // Book the RAM we never get to allocate (EE kernel, ELF image, stack) against
    // ps2::heap::MemTag::ElfSys. Must happen before anything touches the heap, so that what the
    // tags add up to stays a faithful picture of the console's 32MB.
    ps2::heap::TagsAddSystemMem();

    // Locate the game data - host: under PCSX2, USB mass: on a real console (which needs
    // the IOP module bring-up) - before Host_Init, whose COM_InitFilesystem opens the pak
    // files. A build with -DPS2_FS_BASE_PATH=\"...\" pins the base path and skips the
    // detection, for debugging.
#ifdef PS2_FS_BASE_PATH
    const char * const basedir = PS2_FS_BASE_PATH;
#else // PS2_FS_BASE_PATH
    const char * const basedir = ps2::sys::DetectBasePathAndBootIop((argc > 0) ? argv[0] : nullptr);
#endif // PS2_FS_BASE_PATH

    BuildArgv(argc, argv);

    host_parms       = &s_parms;
    s_parms.basedir  = basedir;
    s_parms.argc     = s_argc;
    s_parms.argv     = s_argv;
    s_parms.errstate = 0;

    COM_InitArgv(s_parms.argc, s_parms.argv);
    isDedicated = (COM_CheckParm("-dedicated") != 0);

    Sys_Init();
    Sys_Printf("==== Initializing PS2 QuakeSpasm v%s ====\n", QUAKESPASM_VER_STRING);

    // Cache-line aligned: every hunk block is then 16-byte aligned (the hunk's headers and
    // allocation sizes keep to 16), which the GS needs of the BSP textures it uploads in place.
    s_parms.memsize = HunkSizeBytes();
    s_parms.membase = ps2::heap::AllocAligned(ps2::heap::MemAlign(64), static_cast<size_t>(s_parms.memsize),
                                              ps2::heap::MemTag::Hunk);

    Sys_Printf("Calling Host_Init...\n");
    Host_Init();

    double oldtime = Sys_DoubleTime();
    for (;;)
    {
        const double newtime = Sys_DoubleTime();

        // Host_Frame adds the delta to realtime and runs a frame once enough has gone by
        // (host_maxfps), so it is fed every pass, frame or not. The scope charges the whole of
        // it - server, client, renderer and the vsync wait - to the frame GL_BeginRendering
        // rolls the profiler over at.
        {
            PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Frame);
            Host_Frame(static_cast<float>(newtime - oldtime));
        }

        // Deliberately outside the scope above: the dump is tens of milliseconds of stdout and
        // must not land in the timings it is reporting. The frame it stretches is dropped
        // rather than logged (see FrameLogFlush).
        ps2::debug::FrameLogFlush();

        oldtime = newtime;
    }
}

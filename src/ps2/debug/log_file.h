#pragma once
/* ================================================================================================
 * File: log_file.h
 * Brief: The console log on the game data's drive. Every line the engine prints (Sys_Printf, so
 *        every Con_Printf) and the fatal error go to "<base path>/quake.log", next to id1/. A run
 *        on the console has no stdout anyone can read, so this is how a hardware session gets a
 *        log. Each line is on the drive before the next one is printed, so a hang or a crash
 *        still leaves the log up to that point.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

namespace ps2::debug {

// Points the log at "<basePath>/quake.log". Called once, from main() as soon as the game data
// has been found, so the log covers the whole of Host_Init. The file is emptied by the first
// line written to it, and a file that can't be created leaves the log off, with a warning on
// stdout.
//
// Until the log is on (ps2_logfile), what would have gone into it is kept in RAM, its first
// 16 KB, and written ahead of the first line it logs: the configs that can turn it on run only
// once Host_Init is done, and a log turned on from autoexec.cfg or the console still starts at
// boot.
void LogFileOpen(const char * basePath);

// Registers ps2_logfile, which turns the log off and on (from VID_Init, with the backend's other
// cvars). Until then the log follows the cvar's default.
void LogFileRegisterCvar();

// Appends text, stamping each line with the seconds since main(). HDD/PFS writes sync after
// closing the file; host:/USB writes only close. Callable from any thread.
void LogFileWrite(const char * text);

// printf into the log alone, for what already reached stdout when it happened: the IOP boot
// report, written once the log has its path.
void LogFilePrintf(const char * format, ...) Q_PRINTF_FUNC(1, 2);

// LogFileWrite for the fatal paths (Sys_Error, the EE exception report). These can run on a
// thread whose own write the failure interrupted, still holding the log's lock, so this waits
// for the lock only when another thread holds it. It also starts on a fresh line, and it writes
// whether or not ps2_logfile is on: with the log off, quake.log then holds the backlog (the boot
// report and the last lines printed) and the fatal report alone.
void LogFileWriteFatal(const char * text);

// printf for the reports printed on the way to a fatal error: the render pipeline dump, the
// stack trace, the heap's out-of-memory stats. To stdout and to the log, written as
// LogFileWriteFatal writes, and not to the game console, which these can't count on. A hardware
// run has no stdout anyone reads, so a report printed only there is lost. Each call should end
// its line(s). Not reentrant: one fatal report at a time.
void DumpPrintf(const char * format, ...) Q_PRINTF_FUNC(1, 2);

} // namespace ps2::debug

/* ================================================================================================
 * File: readahead_stream.h
 * Brief: A sequential file stream the background loader (async_io.h) fills ahead of its reader:
 *        what a demo (.dem) is read through, so its frames never wait on the drive. The stdio
 *        stream CL_GetDemoMessage used to fread from refilled its buffer on the main thread, and
 *        a deflated pkz entry inflated through a 32 KB window as it was read: on the Quake II
 *        port's console each 64 KB refill of a demo froze a frame for 80 ms from the HDD, and a
 *        cinematic's for 93 ms from a USB stick, past audsrv's 106 ms ring.
 *
 *        The stream is an fopencookie FILE*, unbuffered, so cl_demo.c's fscanf/fgetc/fread/fclose
 *        work unchanged (the pkz deflated-stream pattern). Behind it a ring of 32 KB slots, each
 *        one loader request at Priority::Stream covering one chunk of the file: all of them in
 *        flight from the open, the consumed head slot resubmitted for the next range. A read
 *        copies from the head slot and sleeps only when it has not landed yet, which on a USB
 *        stick means the loader fell more than the ring behind. A demo reads ~3 KB/s, so its
 *        ring is 64 KB, what the stdio buffer it replaces held; ps2_readahead_kb caps a stream's
 *        ring for anything else that may come to use one. `make pkz` stores the demos (pakzip
 *        --store-ext .dem) so that a pkz's are eligible: a deflated entry has no byte range to
 *        read and keeps the plain stream.
 *
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_FS_READAHEAD_STREAM_H
#define PS2_FS_READAHEAD_STREAM_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct fs_location_s;

/* Whether COM_FOpenFile should try the read-ahead stream for 'filename': a .dem, with
   ps2_readahead_kb above 0. */
int PS2_WantsReadAheadStream(const char * filename);

/* Opens the stream on a located file (COM_LocateFile: a pak range, a stored pkz entry or a loose
   file; a deflated entry can't be read by range and is refused). On success the stream owns the
   location (a loose file's FILE* is closed with it). NULL when the loader is off, the ring can't be
   allocated or the kind is unsuitable: the caller then opens the plain stdio stream as before.
   'filename' is for messages. */
FILE * PS2_OpenReadAheadStream(const char * filename, const struct fs_location_s * loc);

#ifdef __cplusplus
}

namespace ps2::fs {

// Registers ps2_readahead_kb. VID_Init calls it, with the loader's cvars.
void RegisterReadAheadCvar();

} // namespace ps2::fs
#endif /* __cplusplus */

#endif /* PS2_FS_READAHEAD_STREAM_H */

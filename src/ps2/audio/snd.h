#pragma once
/* ================================================================================================
 * File: snd.h
 * Brief: What the rest of the backend asks of the sound output seam (snd.cpp) beyond QuakeSpasm's
 *        SNDDMA_* functions: the feeder thread's measurements, for the frame log.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::audio {

// The ReadCycles() the feeder spent inside audsrv's two RPCs since the last call, then starts
// over: the free-space query (audsrv_available) and the plays (audsrv_play_audio). Either waits on
// the IOP while a file read is out there (audio.md): ps2sdk's audsrv answered at fileXio's
// priority, and from the HDD a query waited up to 65 ms behind a read, most of the ring. The
// game's audsrv answers one priority above, and these show whether it still does.
void TakeFeederRpcCycles(u32 & queryCycles, u32 & playCycles);

} // namespace ps2::audio

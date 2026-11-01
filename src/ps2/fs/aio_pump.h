#pragma once
/* ================================================================================================
 * File: aio_pump.h
 * Brief: Where the background loader's finished requests become engine assets. The loader
 *        (async_io.h) only lands bytes; each client (sounds, and the alias models) keeps its own
 *        table of requests in flight and finishes them on the main thread, from here, once a
 *        frame, under a time budget (ps2_aio_pump_us). The pump runs just before Host_Frame, so
 *        what landed in the last frame's idle time is in place before this frame's draws and its
 *        S_Update. Sounds go first: a landed sound is mixed by the same frame.
 *
 *        Not from inside the frame bracket (GL_BeginRendering): a client's finish may Con_Printf,
 *        which redraws the screen while the client is not in a game, and no frame may open inside
 *        another (gs-renderer.md).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::aio {

// Registers ps2_aio_pump_us. VID_Init calls it, with the loader's cvars.
void RegisterPumpCvar();

// Finishes landed requests, clients in order, until the frame's budget is spent. Main thread.
void Pump();

// Finishes every client's requests, waiting for the loader, then waits out whatever else is in
// flight (aio::Drain): nothing asynchronous is left pending or half done. What
// PS2_AsyncIoDrain does, for the C++ side.
void DrainAll();

} // namespace ps2::aio

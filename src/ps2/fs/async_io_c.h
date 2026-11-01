/* ================================================================================================
 * File: async_io_c.h
 * Brief: The background loader's hooks for QuakeSpasm's C (async_io.h is the C++ API). common.c
 *        calls PS2_AsyncIoDrain before it closes a pack, and gl_model.c at every level change: a
 *        request may still be reading through a pack, or landing for a cache slot the load is
 *        about to sweep. Implemented in fs/aio_pump.cpp.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_FS_ASYNC_IO_C_H
#define PS2_FS_ASYNC_IO_C_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Finishes every client's requests (the sounds', the models'), waiting for the loader, then waits
   until no background request is queued or running, so nothing asynchronous is left in flight or
   half done. Call before freeing or closing anything one might still read through or write into. */
void PS2_AsyncIoDrain(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_FS_ASYNC_IO_C_H */

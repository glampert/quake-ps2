/* ================================================================================================
 * File: model_load.h
 * Brief: Alias models reloaded through the background loader (fs/async_io.h). QuakeSpasm keeps
 *        alias models in the cache, where room for something else evicts them, and Mod_Extradata
 *        reads an evicted one back the moment it is drawn: a synchronous file read in the middle of
 *        a frame, 40-90 ms on a console drive. Mod_ExtradataAsync (gl_model.c) instead hands the
 *        file to this, returns NULL, and the draw skips the entity; the read lands in a staging
 *        buffer, and the pump (fs/aio_pump.h) converts it between frames with the loader
 *        Mod_LoadModel would have used (Mod_LoadModelFromBuffer), so the model is back in the cache
 *        a few frames later. Level loads, brush models and sprites are untouched: only an alias
 *        model with no cache goes this way, and only with the loading plaque down.
 *        NOTE: Shared header between C and C++.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */
#ifndef PS2_RENDERER_MODEL_LOAD_H
#define PS2_RENDERER_MODEL_LOAD_H

#include <tamtypes.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct qmodel_s;

/* Queues the read of 'mod''s file. 1 when queued: the model comes back into the cache through the
   pump, and Mod_ExtradataAsync returns NULL meanwhile. 0 when it can't be: the loader is off, the
   loading plaque is up, the file is missing, too large (kMaxAsyncModelBytes), memory or a table
   slot is short - the caller then loads synchronously as before. */
int PS2_ModelLoadAsync(struct qmodel_s * mod);

/* True while a queued read for 'mod' has not been finished yet. */
int PS2_ModelLoadPending(const struct qmodel_s * mod);

/* gl_model.c: Mod_LoadModel's conversion of a file image already in memory, into the cache. Does
   nothing if the model was loaded again meanwhile. */
void PS2_ModelFinishLoad(struct qmodel_s * mod, unsigned char * data, int size, unsigned int path_id);

#ifdef __cplusplus
}

namespace ps2::mod {

/* Finishes the reads that landed, oldest first, until 'budgetCycles' of ReadCycles() are spent (at
   least one when any has landed). Returns how many were finished. Main thread, once a frame, from
   aio::Pump. */
int PumpLoads(u32 budgetCycles);

/* Finishes every queued read, waiting for the loader: before anything may sweep the models
   (aio::DrainAll). */
void DrainLoads();

} // namespace ps2::mod
#endif /* __cplusplus */

#endif /* PS2_RENDERER_MODEL_LOAD_H */

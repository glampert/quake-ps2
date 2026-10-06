/* ================================================================================================
 * File: snd.cpp
 * Brief: QuakeSpasm's SNDDMA_* sound output seam (q_sound.h), which its portable mixer
 *        (snd_dma.c, snd_mix.c, snd_mem.c) paints through.
 *
 *        Silent for now: SNDDMA_Init declines, and S_Init carries on without sound, as it does on
 *        a desktop with no audio device. The audsrv IOP driver is brought up here when sound is
 *        ported.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

extern "C" {

qboolean SNDDMA_Init(dma_t * dma)
{
    (void)dma;
    Con_Printf("No sound output on the PS2 yet.\n");
    return false;
}

void SNDDMA_Shutdown() {}

// None of these are called once SNDDMA_Init has declined.
int  SNDDMA_GetDMAPos() { return 0; }
void SNDDMA_LockBuffer() {}
void SNDDMA_Submit() {}
void SNDDMA_BlockSound() {}
void SNDDMA_UnblockSound() {}

} // extern "C"

/* ================================================================================================
 * File: aio_pump.cpp
 * Brief: The per-frame pump, and the drain of every client. See aio_pump.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/fs/aio_pump.h"
#include "ps2/fs/async_io.h"
#include "ps2/fs/async_io_c.h"
#include "ps2/audio/sound_load.h"
#include "ps2/renderer/model_load.h"
#include "ps2/debug/profile.h" // kNominalCyclesPerMillisec

namespace ps2::aio {

// The per-frame budget, in microseconds, for turning landed reads into assets. A client finishes
// at least one landed request even when the budget is spent, so nothing waits forever behind a
// long frame.
static cvar_t s_pumpBudgetUs = ps2::MakeCvar("ps2_aio_pump_us", "2000", CVAR_NONE);

void RegisterPumpCvar()
{
    Cvar_RegisterVariable(&s_pumpBudgetUs);
}

void Pump()
{
    if (!Enabled())
    {
        return;
    }

    const u32 budgetCycles = static_cast<u32>(s_pumpBudgetUs.value) * (debug::kNominalCyclesPerMillisec / 1000u);
    audio::PumpLoads(budgetCycles);
    mod::PumpLoads(budgetCycles);
}

void DrainAll()
{
    audio::DrainLoads();
    mod::DrainLoads();
    Drain();
}

} // namespace ps2::aio

// ------------------------------------------------------------------------------------------------
// C hooks (async_io_c.h)
// ------------------------------------------------------------------------------------------------

extern "C" void PS2_AsyncIoDrain(void)
{
    ps2::aio::DrainAll();
}

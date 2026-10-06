/* ================================================================================================
 * File: net.cpp
 * Brief: The network driver table QuakeSpasm's net_main.c iterates, with the loopback driver as
 *        its only entry. A single-player game is a local listen server talking to its own client
 *        through net_loop.c, which is all the PS2 needs; QuakeSpasm's desktop table also lists
 *        the datagram (UDP) driver, which the PS2 build leaves out.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

extern "C" {
    #include "quake/arch_def.h"
    #include "quake/net_sys.h"
    #include "quake/net_defs.h"
    #include "quake/net_loop.h"
}

extern "C" {

net_driver_t net_drivers[] = {
    {
        "Loopback",
        false,
        Loop_Init,
        Loop_Listen,
        Loop_SearchForHosts,
        Loop_Connect,
        Loop_CheckNewConnections,
        Loop_GetMessage,
        Loop_SendMessage,
        Loop_SendUnreliableMessage,
        Loop_CanSendMessage,
        Loop_CanSendUnreliableMessage,
        Loop_Close,
        Loop_Shutdown,
    },
};

// Declared extern by net_defs.h, which is what gives this const external linkage in C++.
const int net_numdrivers = ps2::ArrayLength(net_drivers);

} // extern "C"

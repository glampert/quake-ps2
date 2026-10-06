#pragma once
/* ================================================================================================
 * File: common.h
 * Brief: The single seam between the modern C++ PS2 backend and QuakeSpasm's C. Backend .cpp
 *        files include THIS rather than reaching into the engine headers directly, so the
 *        C-linkage wrapping and the few legacy-header workarounds live in exactly one place.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

// C++ standard headers must be included OUTSIDE the extern "C" block below.
#include <cstddef>
#include <cstdint>

// ps2::heap / MemTags
#include "ps2/system/heap.h"

// QuakeSpasm's engine API - quakedef.h includes every engine header - given C linkage so the
// statically linked engine (compiled as C) and this backend (compiled as C++) agree on unmangled
// symbol names. QuakeSpasm's headers are not held to the backend's strict warning set, and the
// pragmas keep them out of it for this include only.
//
// QuakeSpasm's bspfile.h and ps2sdk's libdraw (draw_buffers.h) both name a type texinfo_t: the
// BSP's on-disk texture info and libdraw's texture buffer info. The backend needs libdraw's and
// has no use for the BSP one (gl_model.c reads it into mtexinfo_t), so QuakeSpasm's is renamed,
// for C++ only. A type name is not part of a C function's symbol, so the engine and the backend
// still agree on every symbol.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#define texinfo_t q1_texinfo_t
extern "C" {
    #include "quake/quakedef.h"
}
#undef texinfo_t
#pragma GCC diagnostic pop

// Helper macros the backend used to take from Quake II's q_shared.h.
#define Q_COLD_FUNC                       __attribute__((cold))
#define Q_ALWAYS_INLINE                   inline __attribute__((always_inline))
#define Q_PRINTF_FUNC(fmtIndex, varIndex) __attribute__((format(printf, fmtIndex, varIndex)))

namespace ps2 {

template<typename T, size_t N>
constexpr int ArrayLength(const T (&)[N])
{
    return static_cast<int>(N);
}

// A QuakeSpasm cvar, ready for Cvar_RegisterVariable. QuakeSpasm's cvars are statically allocated
// structs the engine links into its list, rather than Quake II's Cvar_Get handles, so a backend cvar
// is a namespace-scope `cvar_t` initialized with this. Registering it parses 'value' into the cvar's
// float, so it reads 0 until then. 'name' and 'value' must outlive the cvar (string literals).
constexpr cvar_t MakeCvar(const char * name, const char * value, unsigned int flags)
{
    return cvar_t{ name, value, flags, 0.0f, nullptr, nullptr, nullptr };
}

} // namespace ps2

// Helper assert macros that display the error on screen and halt.
// Prefer these over standard assert().
#if PS2_QUAKE_ASSERTS
    #define PS2_Assert(cond)                               \
        do {                                               \
            if (!(cond)) [[unlikely]]                      \
            {                                              \
                Sys_Error("Assert Failed: %s", #cond);     \
            }                                              \
        } while (0)

    #define PS2_AssertMsg(cond, message)                   \
        do {                                               \
            if (!(cond)) [[unlikely]]                      \
            {                                              \
                Sys_Error("Assert Failed: %s", (message)); \
            }                                              \
        } while (0)
#else // PS2_QUAKE_ASSERTS
    #define PS2_Assert(cond)             (void)sizeof(cond)
    #define PS2_AssertMsg(cond, message) (void)sizeof(cond)
#endif // PS2_QUAKE_ASSERTS

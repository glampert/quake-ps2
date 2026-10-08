---
paths:
  - "src/ps2/system/**"
  - "src/ps2/renderer/**"
  - "src/quake/zone.c"
  - "src/quake/quakedef.h"
---

# Memory budget (32 MB EE RAM)

## The picture

- One program-wide dlmalloc heap (`system/heap.h`) with per-tag accounting
  (`ps2::heap::MemTag`). Kernel, ELF image and stack are booked as `MemTag::ElfSys`, so the
  tags add up to the whole 32 MB. dlmalloc's page size is pinned to 4096 because ps2sdk's
  `sysconf` fails (see `ps2-platform.md`).
- **QuakeSpasm's hunk is one `MemTag::Hunk` block**, 16 MB from `main.cpp` (`-heapsize <KB>`
  overrides it). It holds QuakeSpasm's hunk, its zone (`DYNAMIC_SIZE`) and its cache, so what is
  inside is `hunk_print`'s to show, with REMAINING at the end. A map change frees the hunk back to
  the host level before the next map loads (`Host_ClearMemory`), so levels never overlap in it.
- QuakeSpasm's plain `malloc`s (mostly `sv.edicts`: `max_edicts` × the progs' edict size) land
  in dlmalloc without a tag.
- `ps2_memstats` prints the tag table, then dlmalloc's arena, in-use, untagged-malloc and free
  totals. Pair it with `hunk_print`.
- From the Quake II port, for the renderer's allocations outside the hunk: there, the worst
  moment was always a map transition, with the old level's data still resident while the next
  one loaded. Free the old map's render data before the new map's is built. Fixed segregated
  heaps for textures and models were measured there and rejected: a partition must cover
  max(tex+mdl) + max(everything else), which cost about 1.2 MB more than fragmentation did.

## Limits

QuakeSpasm is sized for a PC. Each limit below is cut back to an earlier QuakeSpasm or FitzQuake
value, or to id's original, and tagged `[PS2_QUAKE]` where it is defined (2026-10-06):

| Limit | QuakeSpasm | PS2 | What it cost |
| --- | --- | --- | --- |
| `MAX_MOD_KNOWN` | 4096 | 512 | 2.0 MB of `.bss` |
| `MAX_STATIC_ENTITIES` | 4096 | 512 | 992 KB of `.bss` |
| `max_edicts` (cvar default) | 8192 | 1024 | most of 7.78 MB of `malloc`, and 2 MB of hunk (`cl_entities`) |
| `DYNAMIC_SIZE` (the zone) | 4 MB | 512 KB | hunk |
| `CON_TEXTSIZE` | 1 MB | 64 KB | hunk |
| `DEFAULT_NUM_PARTICLES` | 16384 | 2048 | 704 KB of hunk |
| `cmd_text` (`Cbuf_Init`) | 256 KB | 64 KB | hunk |
| `NET_MAXMESSAGE`, `MAX_MSGLEN`, `MAX_DATAGRAM` | 64000 | 32000 | qsockets, clients and sizebufs on the hunk, two buffers in `.bss`, and `SV_SendClientDatagram`'s stack buffer (half the 128 KB main stack at 64000) |
| `MAX_MODELS` / `MAX_SOUNDS` | 4096 / 2048 | 1024 / 1024 | precache arrays |
| `MAX_VISEDICTS`, `MAX_CHANNELS`, `MAX_SFX`, `MAXALIASVERTS`/`TRIS` | 4096, 1024, 1024, 2400/4096 | 1024, 512, 512, 1024/2048 | tens of KB each |

Measure with `ps2_memstats` and `hunk_print` before raising any of them.

## Measured (debug build, shareware data, sound off, nothing rendered)

- e1m1 before the limits pass: ELF + system 5.75 MB (`.bss` 3.96 MB), hunk 12.42 MB in use,
  untagged malloc 7.78 MB, **2.44 MB** of RAM left.
- After it: ELF + system 2.92 MB (`.bss` 0.99 MB), untagged malloc 1.08 MB, **11.96 MB** left
  with the 16 MB hunk, on every map.
- Hunk in use per shareware map, once the client has connected: start 4.44, e1m1 4.43, e1m2
  4.42, e1m3 4.33, **e1m4 4.71** (the peak), e1m5 4.33, e1m6 3.88, e1m7 2.83, e1m8 3.62 MB.
  About 1.8 MB of it is fixed: zone, progs, sockets, particles, console.
- The 16 MB hunk is a placeholder. The sound cache (which lives in the hunk) and the renderer
  aren't in yet; size it once they are.
- Renderer, outside the hunk, so far: the frame's DMA chain, one 1 MB block
  (`MemTag::Renderer`) that `VID_Init` allocates for the whole run, and the `ps2::tex` pool
  (640 `Texture` slots) in `.bss`. Boot shows 3.13 MB for ELF + system with the 2D in. In
  demo1 `TexImage` holds 197 KB: the 2D pics' copies, the scrap atlases (64 KB each), and
  texmgr's `gltexture_t` records. 11.56 MB of RAM is left (2026-10-06).
- With the world drawing (2026-10-07): the brush surfaces' baked vertices
  (`MemTag::WorldMdl`, 32 bytes each) are 297 KB (e1m7) to 1.10 MB (e1m4), and the lightmap
  atlases (`MemTag::Lightmap`, 64 KB each) 128 to 256 KB. demo1 ran with 9.15 MB of RAM left.
- With the alias models, sprites and particles (2026-10-08), demo1 (e1m3) holds 209 KB of model
  corners (`MemTag::AliasMdl`, 16 bytes a corner, three a triangle: kept for every model ever
  loaded, all 61 shareware ones would be 388 KB) and 1.72 MB of `TexImage`, most of it skin
  copies (a skin is about 60 KB; its glow texture shares the copy, which saved 0.57 MB). 7.3 MB
  of RAM was left. The models' poses are in the hunk's cache, as QuakeSpasm keeps them.
  Cutting the water on a 32-unit grid adds to `WorldMdl`: 1.20 MB on e1m3.
- Load-time and debug-only sources build `-Os` (`SIZE_OPT_CXX_SRC`), which is RAM for level data.

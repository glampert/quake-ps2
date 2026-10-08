#pragma once
/* ================================================================================================
 * File: alias.h
 * Brief: Alias models (.mdl): the monsters, the items, the view weapon. QuakeSpasm's r_alias.c on
 *        the VU1 path, with gl_mesh.c's load-time half (GL_MakeAliasModelDisplayLists) and the
 *        player skins' colours (R_TranslatePlayerSkin).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"

namespace ps2::alias {

// Registers the alias model cvars and builds the shading tables. R_Init calls it.
void Init();

// What every alias model drawn this frame shares: the dynamic lights that are still alive. The
// view calls it once per frame, before it draws any entity.
void BeginFrame();

// Draws one alias model entity (R_DrawAliasModel): culled, its pose lerped and lit, with its
// fullbright texels over it and, with r_shadows, its shadow. 'viewModel' is the view weapon, whose
// depth is squeezed to the near end of the z-buffer so it never pokes into the walls.
void DrawAliasModel(entity_t & e, bool viewModel);

// Forgets the player skin textures, which TexMgr_NewGame has just freed (R_NewGame).
void NewGame();

} // namespace ps2::alias

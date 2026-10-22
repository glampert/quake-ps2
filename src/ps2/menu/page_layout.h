#pragma once
/* ================================================================================================
 * File: page_layout.h
 * Brief: What the backend's menu pages (Video Options, the debug page) share with QuakeSpasm's
 *        Options page: where the plaque and the rows sit, and where the help line under the rows
 *        goes so that it never runs across the plaque.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::menu {

// The QUAKE plaque down the left, where every page of QuakeSpasm's menu draws it.
constexpr int kPlaqueX = 16;
constexpr int kPlaqueY = 4;

// The rows: 8 units apart from y 32, as Options' are.
constexpr int kRowsY     = 32;
constexpr int kRowHeight = 8;

// The row a page's help line goes on, centred under its 'numRows' rows: a row's gap below them,
// or the first row clear of the plaque when that is lower. Centred text of 29 columns or more
// starts left of the plaque's edge, and on the shorter pages ran across its id logo.
inline int HelpLineY(const int numRows, const int plaqueHeight)
{
    const int underRows   = kRowsY + (kRowHeight * (numRows + 1));
    const int plaqueEnd   = kPlaqueY + plaqueHeight;
    const int underPlaque = kRowsY + (kRowHeight * ((plaqueEnd - kRowsY + kRowHeight - 1) / kRowHeight));
    return (underRows > underPlaque) ? underRows : underPlaque;
}

} // namespace ps2::menu

#!/usr/bin/env python3
# ================================================================================================
# File: make_menu_about.py
# Brief: Makes gfx/menuabout.lmp, the main menu's "About" item, out of id1's pak0 (the shareware
#        or the registered one: their menu pictures are the same), in the lettering of the other
#        items in gfx/mainmenu.lmp.
#
# None of the game's menu pictures has an A or a b in that lettering, so this cuts the letters it
# can find and makes the other two from their nearest neighbours: the o of "Join a Game"
# (gfx/mp_menu.lmp), the u and t of "Quit" (gfx/mainmenu.lmp), a capital A from the small a of
# "Join a Game", three of its rows and its apex column repeated so it grows as a capital does,
# and a b from the d of "Load" (gfx/sp_menu.lmp), its bowl drawn twice, one above the other. The
# letters sit on the rows "Help/Ordering" and "Quit" put theirs on, so the item lines up with them.
#
# The ELF carries the result (src/ps2/builtin/menuabout.cpp, made from it by imgdump), as no game
# data has it:
#
#   src/tools/scripts/make_menu_about.py id1/pak0.pak menuabout.lmp
#   build/tools/imgdump --out src/ps2/builtin menuabout.lmp
#
# This source code is released under the GNU GPL v2 license.
# Check the accompanying LICENSE file for details.
# ================================================================================================

"""Make the main menu's About item, gfx/menuabout.lmp, from id1/pak0.pak."""

import argparse
import struct
import sys

TRANSPARENT = 255  # Quake's see-through palette index
OUTLINE = 0x60     # the darkest brown, which every letter is outlined in

ITEM_HEIGHT = 20   # the main menu's items are 20 rows apart


def read_pak(path):
    """The pak's files, by name, as bytes."""
    with open(path, 'rb') as f:
        data = f.read()
    ident, dir_offset, dir_length = struct.unpack_from('<4sii', data, 0)
    if ident != b'PACK':
        sys.exit(f'{path} is not a pak')
    files = {}
    for i in range(dir_length // 64):
        name, offset, length = struct.unpack_from('<56sii', data, dir_offset + i * 64)
        files[name.split(b'\0')[0].decode('ascii')] = data[offset:offset + length]
    return files


class Pic:
    """A gfx/*.lmp: its width and height, then a palette index per pixel, row by row."""

    def __init__(self, data):
        self.width, self.height = struct.unpack_from('<ii', data, 0)
        self.pixels = data[8:8 + self.width * self.height]

    def cut(self, x0, y0, x1, y1):
        """Columns x0 to x1 - 1 of rows y0 to y1 - 1, as a list of rows."""
        return [[self.pixels[y * self.width + x] for x in range(x0, x1)] for y in range(y0, y1)]


def make_b(sp_menu):
    """Load's d with its bowl drawn twice: a stem and two bowls, a small capital B."""
    stem = sp_menu.cut(58, 23, 66, 36)
    # The d's foot runs into the a before it: it gets the left outline its top has.
    stem[11][0] = OUTLINE
    stem[12] = [OUTLINE] * len(stem[12])

    bowl = sp_menu.cut(66, 23, 77, 36)  # a see-through column first, between it and the stem
    bowl[1][7] = TRANSPARENT  # a bright stray pixel past the bowl's outline

    def row(d_row):  # a row of the d's bowl, by its row in sp_menu.lmp
        return list(bowl[d_row - 23])

    # Six rows to a bowl, from the top of the d's, its widest and its bottom, with an outline at
    # the waist as at the top and the bottom: 13 rows, as tall as the d.
    waist = [TRANSPARENT] + [OUTLINE] * 6 + [TRANSPARENT] * 4
    bowls = [row(23), row(24), row(25), row(28), row(33), row(34), waist,
             row(24), row(25), row(28), row(33), row(34), row(35)]
    return [stem[r] + bowls[r] for r in range(len(bowls))]


def make_capital_a(mp_menu):
    """Join a Game's small a, three rows down its legs repeated and its apex column with each, so
    that it stands 17 rows tall, as the capitals do, and widens as it goes down."""
    a = mp_menu.cut(71, 1, 90, 15)
    apex = 9
    repeated_rows = (3, 6, 10)

    grown = []  # (columns added so far, row)
    added = 0
    for r, row in enumerate(a):
        for copy in range(2 if r in repeated_rows else 1):
            added += copy
            grown.append((added, row[:apex] + [row[apex]] * (1 + added) + row[apex + 1:]))

    # Each row centred under the widest.
    letter = []
    for added_here, row in grown:
        left = (added - added_here) // 2
        letter.append([TRANSPARENT] * left + row + [TRANSPARENT] * (added - added_here - left))
    return letter


def make_about(files):
    mainmenu = Pic(files['gfx/mainmenu.lmp'])
    sp_menu = Pic(files['gfx/sp_menu.lmp'])
    mp_menu = Pic(files['gfx/mp_menu.lmp'])

    # (letter, its top row): the small letters' tops on row 3, as Quit's are, the A's a row
    # above the other capitals', as the small a's apex is above the small letters.
    letters = [
        (make_capital_a(mp_menu), 0),
        (make_b(sp_menu), 3),
        (mp_menu.cut(14, 2, 35, 15), 3),    # the o of Join
        (mainmenu.cut(23, 83, 42, 96), 3),  # the u of Quit
        (mainmenu.cut(52, 83, 71, 99), 3),  # the t of Quit, its stem down past the line
    ]

    # A column between letters, as the menu's own have. The width rounds up to 16, which the PS2
    # uploads a pic in place at (draw.cpp's MakeBuiltin).
    x = 1
    placed = []
    for letter, top in letters:
        placed.append((letter, x, top))
        x += len(letter[0]) + 1
    width = (x + 15) & ~15

    image = [[TRANSPARENT] * width for _ in range(ITEM_HEIGHT)]
    for letter, left, top in placed:
        for r, row in enumerate(letter):
            for c, index in enumerate(row):
                if index != TRANSPARENT:
                    image[top + r][left + c] = index
    return width, ITEM_HEIGHT, bytes(index for row in image for index in row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pak0', help="id1's pak0.pak")
    parser.add_argument('out', nargs='?', default='menuabout.lmp', help='the .lmp to write')
    args = parser.parse_args()

    width, height, pixels = make_about(read_pak(args.pak0))
    with open(args.out, 'wb') as f:
        f.write(struct.pack('<ii', width, height) + pixels)
    print(f'Wrote {args.out}: {width}x{height}')


if __name__ == '__main__':
    main()

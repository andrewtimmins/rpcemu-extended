#!/usr/bin/env python3
#
# RPCEmu - An Acorn system emulator
#
# Copyright (C) 2026 Andy Timmins
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
"""Generate the !Sprites files for the bundled !Packages template.

The icons are drawn here rather than committed as opaque binaries with no
source, which is how the two sprite files already in riscos-progs/ arrived and
why neither can be altered without starting again. Run this and commit the
result; nothing in the build depends on Python.

    tools/riscos/mksprites.py [output-dir]

Three files, because RISC OS picks between them by the display's eigen factors,
and the geometry of each is what a stock PackMan installation uses:

    !Sprites     mode 15, pixels twice as tall as wide   34x17, 17x9
    !Sprites11   mode word, 8bpp, 180dpi square          68x68, 34x34
    !Sprites22   mode 21, square pixels                  34x34, 17x17

The shape is computed from the canvas rather than drawn as pixel art, so the
same parcel comes out at every size, and the vertical squash of the mode 15 set
falls out of deriving the y measurements from the height.

Sprite format, verified against a real PackMan installation before being
written here (see docs/packages.md):

  A sprite FILE is a sprite area control block minus its first word, so file
  offset f is area offset f+4. That is why "offset to first sprite" is 16 while
  the first sprite starts 12 bytes into the file.

  Each sprite carries its own 256-entry palette, which is what makes the colours
  ours rather than whatever the screen mode happens to be showing. Two words per
  entry, both the same unless the colour flashes, each &BBGGRR00.

  The mask differs between the two sprite formats, and getting it wrong is the
  one mistake that produces a plausible file RISC OS draws as garbage:
    - old format (a mode NUMBER):   same depth as the image, &00 or &FF
    - new format (a mode WORD):     one bit per pixel, least significant first
"""

import os
import struct
import sys

# Palette index 0 is never drawn: it is what the mask leaves out. Its colour is
# arbitrary and only matters to a tool that ignores the mask.
TRANSPARENT = 0
OUTLINE     = 1
FRONT       = 2
TOP         = 3
SIDE        = 4
TAPE        = 5
TAPE_TOP    = 6

PALETTE = {
    TRANSPARENT: (0x00, 0x00, 0x00),
    OUTLINE:     (0x3a, 0x24, 0x14),
    FRONT:       (0xc8, 0x9a, 0x5c),
    TOP:         (0xe2, 0xba, 0x82),
    SIDE:        (0xa0, 0x76, 0x40),
    TAPE:        (0xf0, 0xe4, 0xc8),
    TAPE_TOP:    (0xff, 0xf4, 0xdc),
}


class Canvas:
    """A paletted bitmap. Pixel 0 is transparent, which is also the mask."""

    def __init__(self, width, height):
        self.w = width
        self.h = height
        self.px = bytearray(width * height)

    def set(self, x, y, colour):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = colour

    def fill_poly(self, points, colour):
        """Scanline fill, even-odd. Enough for the convex quads used here."""
        ys = [p[1] for p in points]
        for y in range(max(0, min(ys)), min(self.h - 1, max(ys)) + 1):
            crossings = []
            for i in range(len(points)):
                x0, y0 = points[i]
                x1, y1 = points[(i + 1) % len(points)]
                if y0 == y1:
                    continue
                if min(y0, y1) <= y < max(y0, y1):
                    t = (y - y0) / (y1 - y0)
                    crossings.append(x0 + t * (x1 - x0))
            crossings.sort()
            for i in range(0, len(crossings) - 1, 2):
                for x in range(int(round(crossings[i])),
                               int(round(crossings[i + 1])) + 1):
                    self.set(x, y, colour)

    def line(self, x0, y0, x1, y1, colour):
        """Bresenham, so a sheared edge has no gaps."""
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.set(x0, y0, colour)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy


def parcel(width, height):
    """A taped parcel, in cabinet projection, filling the canvas.

    Every x measurement comes from the width and every y from the height, which
    is what makes the mode 15 set (half the height, same width) come out as the
    same parcel rather than a squashed drawing of a different one.
    """
    c = Canvas(width, height)

    dx = max(1, round(width * 0.20))	# depth of the projection
    dy = max(1, round(height * 0.20))

    left = 1
    right = width - 1 - dx
    top = dy + 1			# top edge of the FRONT face
    bottom = height - 2

    front = [(left, top), (right, top), (right, bottom), (left, bottom)]
    top_face = [(left, top), (left + dx, top - dy),
                (right + dx, top - dy), (right, top)]
    side_face = [(right, top), (right + dx, top - dy),
                 (right + dx, bottom - dy), (right, bottom)]

    c.fill_poly(front, FRONT)
    c.fill_poly(side_face, SIDE)
    c.fill_poly(top_face, TOP)

    # The tape, down the front and on over the lid.
    tape_w = max(2, round(width * 0.13))
    mid = left + (right - left) // 2
    tx0 = mid - tape_w // 2
    tx1 = tx0 + tape_w - 1

    c.fill_poly([(tx0, top), (tx1, top), (tx1, bottom), (tx0, bottom)], TAPE)
    c.fill_poly([(tx0, top), (tx0 + dx, top - dy),
                 (tx1 + dx, top - dy), (tx1, top)], TAPE_TOP)

    # Outlines last, so nothing fills over them.
    for a, b in ((front[0], front[1]), (front[1], front[2]),
                 (front[2], front[3]), (front[3], front[0]),
                 (top_face[1], top_face[2]), (top_face[0], top_face[1]),
                 (top_face[2], top_face[3]),
                 (side_face[2], side_face[3]), (side_face[1], side_face[2])):
        c.line(a[0], a[1], b[0], b[1], OUTLINE)

    return c


def palette_bytes():
    out = bytearray()
    for i in range(256):
        r, g, b = PALETTE.get(i, (0, 0, 0))
        word = (b << 24) | (g << 16) | (r << 8)
        out += struct.pack('<II', word, word)	# both flash colours the same
    return bytes(out)


def sprite(name, canvas, mode, new_format):
    """One sprite: 44-byte header, palette, 8bpp image, mask."""
    words = (canvas.w * 8 + 31) // 32		# 8bpp, so 4 pixels per word
    stride = words * 4

    image = bytearray()
    for y in range(canvas.h):
        row = canvas.px[y * canvas.w:(y + 1) * canvas.w]
        image += row + bytes(stride - len(row))

    mask = bytearray()
    if new_format:
        mask_stride = (canvas.w + 31) // 32 * 4
        for y in range(canvas.h):
            row = bytearray(mask_stride)
            for x in range(canvas.w):
                if canvas.px[y * canvas.w + x] != TRANSPARENT:
                    row[x // 8] |= 1 << (x % 8)
            mask += row
    else:
        for y in range(canvas.h):
            row = bytearray(stride)
            for x in range(canvas.w):
                if canvas.px[y * canvas.w + x] != TRANSPARENT:
                    row[x] = 0xff
            mask += row

    pal = palette_bytes()
    image_off = 44 + len(pal)
    mask_off = image_off + len(image)
    total = mask_off + len(mask)

    encoded = name.encode('latin1')
    if len(encoded) > 12:
        raise ValueError('sprite name "%s" is longer than 12 characters' % name)

    header = struct.pack('<I12sIIIIIII',
                         total,
                         encoded,			# zero padded by the 12s
                         words - 1,
                         canvas.h - 1,
                         0,				# first bit used
                         (canvas.w * 8 - 1) % 32,	# last bit used
                         image_off,
                         mask_off,
                         mode)
    assert len(header) == 44
    return header + pal + bytes(image) + bytes(mask)


def sprite_file(sprites):
    body = b''.join(sprites)
    # A file is the area control block without its size word, so an area offset
    # of 16 is 12 bytes into the file, and the free pointer is the area size.
    return struct.pack('<III', len(sprites), 16, 16 + len(body)) + body


def mode_word(sprite_type, xdpi, ydpi):
    return 1 | (xdpi << 1) | (ydpi << 14) | (sprite_type << 27)


SPRITE_TYPE_8BPP = 4

# leaf, mode, new format, large size, small size
SETS = (
    ('!Sprites',   15,                                    False, (34, 17), (17, 9)),
    ('!Sprites11', mode_word(SPRITE_TYPE_8BPP, 180, 180), True,  (68, 68), (34, 34)),
    ('!Sprites22', 21,                                    False, (34, 34), (17, 17)),
)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', '..', 'resources', 'ro5', 'packages')
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    for leaf, mode, new_format, large, small in SETS:
        data = sprite_file((
            sprite('!packages', parcel(*large), mode, new_format),
            sprite('sm!packages', parcel(*small), mode, new_format),
        ))
        path = os.path.join(out_dir, leaf + ',ff9')
        with open(path, 'wb') as f:
            f.write(data)
        print('%-16s %5d bytes  %dx%d and %dx%d' %
              (leaf, len(data), large[0], large[1], small[0], small[1]))


if __name__ == '__main__':
    main()

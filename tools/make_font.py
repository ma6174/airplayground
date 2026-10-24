#!/usr/bin/env python3
"""Compiles the on-screen bitmap font for the text source (font16.bin).

Renders every glyph of the built-in charset from an outline font into an
anti-aliased 18x20 cell (4 bits of coverage per pixel, the extra width
covers the widest hanzi, the extra rows hold Latin descenders), with the
per-glyph advance width so Latin text gets proportional spacing. The
cells are packed into the flat file the firmware's media_text module maps.
The file is committed to the tree, so firmware builds and host tests never
need the outline font.

Charset: printable ASCII, the GB2312 full-width punctuation row, and the
GB2312 level-1 hanzi (the ~3500 most common characters). Glyphs the input
font lacks are reported and skipped; text the firmware cannot find in the
font is simply not drawn.

The committed font was generated from Noto Sans SC (SIL Open Font License
1.1), which permits redistribution of the rendered bitmap subset:
  https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf

Regenerate with Pillow installed:
  python3 tools/make_font.py --font NotoSansSC.ttf \
      --out firmware/components/media/font16.bin

File format (integers little-endian, coverage nibbles low-pixel-first):
  "APF2" | u8 cell_w | u8 cell_h
  | u16 run_first | u16 run_count | u16 table_count | u16 zero | u16 pad
  (the header is 16 bytes)
  | run_count x (u8 advance + cell_w/2*cell_h bytes)
    (code points run_first..+run_count)
  | table_count x u16                  (sorted code points)
  | table_count x (u8 advance + cell_w/2*cell_h bytes)
"""

import argparse
import hashlib
import struct

from PIL import Image, ImageDraw, ImageFont

FONT_SIZE = 16  # outline size the glyphs are rendered at
DEFAULT_WEIGHT = 500  # variable-font axis value; heavier reads better on a TV
CELL_W = 18  # cell width: the widest ink at 16 px measures 17 px (e.g. 九, 丸)
CELL_H = 20
# Noto Sans SC at 16 px keeps CJK ink roughly rows 5..21 and Latin descenders
# down to row 23, so the draw origin moves up 5 rows to fit every glyph in
# the 20-row cell. The clip warning below catches fonts that need a different
# offset.
DRAW_DY = -5
BLANK_OK = {0x20, 0x3000}


def charset():
    points = set(range(0x20, 0x7F))

    def gb_row(high):
        for low in range(0xA1, 0xFF):
            try:
                points.add(ord(bytes([high, low]).decode("gb2312")))
            except UnicodeDecodeError:
                pass  # unused code position

    gb_row(0xA1)  # full-width punctuation and symbols
    for high in range(0xB0, 0xD8):  # level-1 hanzi, GB2312 rows 16..55
        gb_row(high)
    return sorted(points)


def render(font, text):
    """Returns (advance, cell bytes, clipped) for one character."""
    canvas = Image.new("L", (CELL_W * 2, CELL_H * 2), 0)
    ImageDraw.Draw(canvas).text((0, DRAW_DY), text, fill=255, font=font)
    bbox = canvas.getbbox()
    clipped = bbox is not None and (bbox[2] > CELL_W or bbox[3] > CELL_H)
    cell = canvas.crop((0, 0, CELL_W, CELL_H))
    # The advance is the typographic width, independent of the ink: glyphs
    # draw into their full cell, spacing follows this value.
    advance = max(1, min(CELL_W, round(font.getlength(text))))
    packed = bytearray((CELL_W // 2) * CELL_H)
    for y in range(CELL_H):
        for x in range(CELL_W):
            alpha = cell.getpixel((x, y)) >> 4
            if alpha:
                byte = y * (CELL_W // 2) + x // 2
                if x & 1:
                    packed[byte] |= alpha  # low nibble: right pixel
                else:
                    packed[byte] |= alpha << 4
    return advance, bytes(packed), clipped


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--font", required=True, help="outline font (TTF/OTF)")
    parser.add_argument("--out", required=True, help="output path for the .bin")
    parser.add_argument("--weight", type=int, default=DEFAULT_WEIGHT,
                        help="variable-font weight axis (default %(default)s)")
    args = parser.parse_args()

    font = ImageFont.truetype(args.font, FONT_SIZE)
    try:
        font.set_variation_by_axes([args.weight])
    except (OSError, ValueError):
        pass

    run_first = 0x20
    run = []  # (advance, cell) for printable ASCII, one fixed-width block
    table = []  # (code point, advance, cell) for everything else, sorted
    dropped = []
    clipped = 0
    for cp in charset():
        advance, glyph, over = render(font, chr(cp))
        if over:
            clipped += 1
        if not any(glyph) and cp not in BLANK_OK:
            dropped.append(cp)
            continue
        if 0x20 <= cp < 0x20 + 95:
            run.append((advance, glyph))
        else:
            table.append((cp, advance, glyph))

    cell_bytes = (CELL_W // 2) * CELL_H
    blob = bytearray()
    blob += b"APF2"
    blob += struct.pack("<BB", CELL_W, CELL_H)
    blob += struct.pack("<HHHH", run_first, len(run), len(table), 0)
    blob += b"\x00\x00"  # pad the header to 16 bytes, a round number
    for advance, glyph in run:
        blob += struct.pack("<B", advance) + glyph
    for cp, _, _ in table:
        blob += struct.pack("<H", cp)
    for _, advance, glyph in table:
        blob += struct.pack("<B", advance) + glyph

    with open(args.out, "wb") as out:
        out.write(blob)

    print(f"wrote {args.out}: {len(blob)} bytes, sha256 {hashlib.sha256(blob).hexdigest()[:16]}")
    print(f"glyphs: {len(run)} ascii + {len(table)} table = {len(run) + len(table)}, "
          f"{cell_bytes + 1} B per glyph")
    if clipped:
        print(f"note: {clipped} glyphs exceeded the {CELL_W}x{CELL_H} cell and were clipped")
    if dropped:
        names = " ".join(f"U+{cp:04X}" for cp in dropped[:20])
        print(f"missing from the input font ({len(dropped)}): {names}")


if __name__ == "__main__":
    main()

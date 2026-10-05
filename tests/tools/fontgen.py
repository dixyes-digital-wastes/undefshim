#!/usr/bin/env python3
"""Turn an Atari font's outlines into a glyph table the driver can draw with.

The screen is the only channel left when the UART is not configured or is not
mapped, so the error paths need to draw text themselves. Drawing needs a font,
and a font is a table of small integers nobody wants to read or maintain by
hand, so it is generated once and the result is committed: the build then
needs nothing but what is in the tree.

    tests/tools/fontgen.py \
        "third_party/EightBit-Atari-Fonts/Truetype/EightBit Atari-90.ttf" \
        uefi/font.h \
        "third_party/EightBit-Atari-Fonts/Original Files/PNG/90.png"

Pillow is needed to run this, and only to run this. It is not needed to build.

The collection holds outlines traced from the original Atari 8-bit bitmaps,
and it holds those bitmaps as well. Rendering the outlines at the size below
reproduces them, which is what the reference image is for: given one, every
glyph is compared against the original and a disagreement fails the run. That
check is the reason the outlines can be trusted at all. An outline font is not
a bitmap, so whether a renderer at a given size lands on the original's pixels
is a fact about the renderer and has to be measured rather than assumed.

The four characters the Atari character set does not have -- the grave, the
braces and the tilde -- have no bitmap to match and come from the outlines
alone. They are reported, not silently accepted.
"""
import sys

# The printable range, which is what a message can be made of
FIRST = 32
LAST = 126

# What the face is rendered at, and where the glyph sits in that image. Both
# are measured: at eight, two rows down, the render lands on the original
# bitmap pixel for pixel, which the reference check confirms. Nothing here
# scales a glyph -- what comes out is what the face draws at the size it draws
# it
SIZE = 8
OFFSET_X = 0
OFFSET_Y = 2

# A pixel is ink when the coverage is past half. These faces are bitmaps
# traced into outlines, so an edge is already where the original's pixel was
INK = 96

# Where the original keeps each printable character. The Atari character set
# puts the printable ones in its first sixty-four cells and the lower case at
# the character's own code, rather than in ASCII's order; this is that layout,
# read off the reference image
def referenceCell(code):
    if FIRST <= code <= 95:
        return code - FIRST
    if 97 <= code <= 122:
        return code
    if code == 124:
        return 124
    return None


def render(font, ch):
    """One glyph, as the bits of an image the face was drawn into."""
    from PIL import Image, ImageDraw

    image = Image.new("L", (SIZE + OFFSET_X + 2, SIZE + OFFSET_Y + 4), 0)
    ImageDraw.Draw(image).text((0, 0), ch, fill=255, font=font)
    rows = []
    for y in range(SIZE):
        bits = 0
        for x in range(SIZE):
            if image.getpixel((x + OFFSET_X, y + OFFSET_Y)) > INK:
                bits |= 0x80 >> x
        rows.append(bits)
    # The glyph has to fit the window it is read out of, in both directions
    for x in range(image.width):
        for y in range(image.height):
            if image.getpixel((x, y)) > INK:
                inside = OFFSET_X <= x < OFFSET_X + SIZE and OFFSET_Y <= y < OFFSET_Y + SIZE
                if not inside:
                    raise SystemExit("%r draws outside the eight by eight a glyph "
                                     "has" % ch)
    return rows


def readReference(path):
    """The original bitmap: 32 by 4 cells of eight by eight, in one image."""
    from PIL import Image

    image = Image.open(path).convert("L")
    cells = []
    for n in range(128):
        x0 = (n % 32) * 8
        y0 = (n // 32) * 8
        cell = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if image.getpixel((x0 + x, y0 + y)) > INK:
                    bits |= 0x80 >> x
            cell.append(bits)
        cells.append(cell)
    return cells


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit("usage: fontgen.py <font.ttf> <out.h> [reference.png]")
    source, target = sys.argv[1], sys.argv[2]
    reference = readReference(sys.argv[3]) if len(sys.argv) == 4 else None

    from PIL import ImageFont

    font = ImageFont.truetype(source, SIZE)

    glyphs = []
    matched = 0
    absent = []
    wrong = []
    for code in range(FIRST, LAST + 1):
        rows = render(font, chr(code))
        if not any(rows) and code != FIRST:
            raise SystemExit("%r came out blank" % chr(code))
        glyphs.append((code, rows))
        if reference is None:
            continue
        at = referenceCell(code)
        if at is None:
            absent.append(chr(code))
        elif reference[at] == rows:
            matched += 1
        else:
            wrong.append(chr(code))

    if reference is not None:
        print("%d glyphs match the original bitmap" % matched)
        print("%d have none: %s" % (len(absent), " ".join(absent)))
        if wrong:
            raise SystemExit("these do not match the original: %s"
                             % " ".join(wrong))
        if matched + len(absent) != LAST - FIRST + 1:
            raise SystemExit("only %d of the %d glyphs were accounted for"
                             % (matched + len(absent), LAST - FIRST + 1))

    out = []
    out.append("/*")
    out.append(" * The glyphs the screen drawer uses, for ASCII %d to %d" % (FIRST, LAST))
    out.append(" *")
    out.append(" * Generated by tests/tools/fontgen.py from the outlines of EightBit")
    out.append(" * Atari-90, at the height that face draws at, which is the height of")
    out.append(" * the original Atari bitmap it was traced from. Committed rather than")
    out.append(" * generated at build time so that a build needs nothing but this tree")
    out.append(" *")
    out.append(" * One byte per row, most significant bit on the left, %d rows a glyph," % SIZE)
    out.append(" * the first row at the top. This file is not meant to be edited")
    out.append(" */")
    out.append("")
    out.append("#ifndef US_FONT_H")
    out.append("#define US_FONT_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define US_FONT_FIRST %d" % FIRST)
    out.append("#define US_FONT_LAST %d" % LAST)
    out.append("#define US_FONT_WIDTH %d" % SIZE)
    out.append("#define US_FONT_HEIGHT %d" % SIZE)
    out.append("")
    out.append("static const uint8_t kFont[US_FONT_LAST - US_FONT_FIRST + 1][US_FONT_HEIGHT] = {")
    for code, rows in glyphs:
        cells = ", ".join("0x%02x" % value for value in rows)
        shown = "space" if code == FIRST else chr(code)
        out.append("    { %s }, /* %s */" % (cells, shown))
    out.append("};")
    out.append("")
    out.append("#endif")
    with open(target, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s: %d glyphs, %dx%d" % (target, len(glyphs), SIZE, SIZE))


if __name__ == "__main__":
    main()

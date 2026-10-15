#!/usr/bin/env python3
"""Merges HVIF icons into one: the later icons are drawn over the earlier.

    merge_hvif.py OUTPUT BASE OVERLAY...

Made for Haiku's icon set, whose overlay icons (Overlay_Be, ...) share the
64 x 64 space of the icons they go on. The styles, paths and shapes of
each icon are appended, and each shape's style and path indices are moved
past those of the icons before it. The format follows
src/libs/icon/flat_icon/FlatIconImporter.cpp in the Haiku source tree (see
also tools/make_icon.py).
"""

import sys

MAGIC = b"ncif"

STYLE_SOLID_COLOR = 1
STYLE_GRADIENT = 2
STYLE_SOLID_COLOR_NO_ALPHA = 3
STYLE_SOLID_GRAY = 4
STYLE_SOLID_GRAY_NO_ALPHA = 5
GRADIENT_FLAG_TRANSFORM = 1 << 1
GRADIENT_FLAG_NO_ALPHA = 1 << 2
GRADIENT_FLAG_GRAYS = 1 << 4

PATH_FLAG_USES_COMMANDS = 1 << 2
PATH_FLAG_NO_CURVES = 1 << 3
PATH_COMMAND_SIZES = (1, 1, 2, 6)		# h line, v line, line, curve

SHAPE_PATH_SOURCE = 10
SHAPE_FLAG_TRANSFORM = 1 << 1
SHAPE_FLAG_LOD_SCALE = 1 << 3
SHAPE_FLAG_HAS_TRANSFORMERS = 1 << 4
SHAPE_FLAG_TRANSLATION = 1 << 5
TRANSFORMER_SIZES = {20: 18, 21: 3, 23: 3}	# affine, contour, stroke

MATRIX_SIZE = 6 * 3						# six 24-bit floats


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def byte(self):
        if self.pos >= len(self.data):
            raise ValueError("truncated icon")
        value = self.data[self.pos]
        self.pos += 1
        return value

    def skip(self, count):
        if self.pos + count > len(self.data):
            raise ValueError("truncated icon")
        self.pos += count

    def coords(self, count):
        for _ in range(count):
            if self.byte() & 0x80:
                self.skip(1)


def skip_style(reader):
    kind = reader.byte()
    if kind == STYLE_SOLID_COLOR:
        reader.skip(4)
    elif kind == STYLE_SOLID_COLOR_NO_ALPHA:
        reader.skip(3)
    elif kind == STYLE_SOLID_GRAY:
        reader.skip(2)
    elif kind == STYLE_SOLID_GRAY_NO_ALPHA:
        reader.skip(1)
    elif kind == STYLE_GRADIENT:
        reader.skip(1)					# gradient type
        flags = reader.byte()
        stops = reader.byte()
        if flags & GRADIENT_FLAG_TRANSFORM:
            reader.skip(MATRIX_SIZE)
        color = 1 if flags & GRADIENT_FLAG_GRAYS else 3
        if not flags & GRADIENT_FLAG_NO_ALPHA:
            color += 1
        reader.skip(stops * (1 + color))
    else:
        raise ValueError("unknown style type %d" % kind)


def skip_path(reader):
    flags = reader.byte()
    points = reader.byte()
    if flags & PATH_FLAG_USES_COMMANDS:
        commands = reader.data[reader.pos:reader.pos + (points + 3) // 4]
        reader.skip((points + 3) // 4)
        for i in range(points):
            command = (commands[i // 4] >> ((i % 4) * 2)) & 3
            reader.coords(PATH_COMMAND_SIZES[command])
    elif flags & PATH_FLAG_NO_CURVES:
        reader.coords(points * 2)
    else:
        reader.coords(points * 6)


def read_shape(reader, style_base, path_base):
    """Returns the shape with its indices moved by the bases."""
    start = reader.pos
    if reader.byte() != SHAPE_PATH_SOURCE:
        raise ValueError("unknown shape type")
    style = reader.byte() + style_base
    count = reader.byte()
    paths = [reader.byte() + path_base for _ in range(count)]
    tail = reader.pos
    flags = reader.byte()
    if flags & SHAPE_FLAG_TRANSFORM:
        reader.skip(MATRIX_SIZE)
    elif flags & SHAPE_FLAG_TRANSLATION:
        reader.coords(2)
    if flags & SHAPE_FLAG_LOD_SCALE:
        reader.skip(2)
    if flags & SHAPE_FLAG_HAS_TRANSFORMERS:
        for _ in range(reader.byte()):
            kind = reader.byte()
            if kind not in TRANSFORMER_SIZES:
                raise ValueError("unsupported transformer type %d" % kind)
            reader.skip(TRANSFORMER_SIZES[kind])
    if max([style] + paths) > 255:
        raise ValueError("too many styles or paths")
    return (bytes([SHAPE_PATH_SOURCE, style, count] + paths)
        + reader.data[tail:reader.pos])


def parse(data, style_base=0, path_base=0):
    """Splits an icon into its styles, paths and (re-indexed) shapes."""
    if data[:4] != MAGIC:
        raise ValueError("not an HVIF icon")
    reader = Reader(data)
    reader.skip(4)
    sections = []
    for skip in (skip_style, skip_path):
        items = []
        for _ in range(reader.byte()):
            start = reader.pos
            skip(reader)
            items.append(data[start:reader.pos])
        sections.append(items)
    styles, paths = sections
    shapes = [read_shape(reader, style_base, path_base)
        for _ in range(reader.byte())]
    if reader.pos != len(data):
        raise ValueError("%d bytes left after the shapes"
            % (len(data) - reader.pos))
    return styles, paths, shapes


def merge(icons):
    styles, paths, shapes = [], [], []
    for data in icons:
        more = parse(data, len(styles), len(paths))
        styles += more[0]
        paths += more[1]
        shapes += more[2]
    if max(len(styles), len(paths), len(shapes)) > 255:
        raise ValueError("too many styles, paths or shapes")
    out = bytearray(MAGIC)
    for items in (styles, paths, shapes):
        out.append(len(items))
        for item in items:
            out += item
    return bytes(out)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    icons = []
    for path in sys.argv[2:]:
        with open(path, "rb") as f:
            icons.append(f.read())
    merged = merge(icons)
    styles, paths, shapes = parse(merged)
    with open(sys.argv[1], "wb") as f:
        f.write(merged)
    print("%s: %d bytes, %d styles, %d paths, %d shapes" % (sys.argv[1],
        len(merged), len(styles), len(paths), len(shapes)))


if __name__ == "__main__":
    main()

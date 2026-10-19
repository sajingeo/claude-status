#!/usr/bin/env python3
"""Embed GIF files as LVGL image descriptors.

Usage:
  python3 -I gen_gif_assets.py <out.c> <out.h> name=path.gif [name=path.gif ...]

Each `name` becomes `gif_<name>` (an lv_image_dsc_t) in the generated C, with
the raw GIF bytes as the backing array. LVGL's GIF decoder reads the real
dimensions from the data, so the header w/h are left 0 (matching LVGL's own
embedded-gif example).
"""
import sys


def main():
    out_c, out_h = sys.argv[1], sys.argv[2]
    items = [a.split("=", 1) for a in sys.argv[3:]]

    with open(out_c, "w") as c:
        c.write('#include "gif_assets.h"\n\n')
        for name, path in items:
            with open(path, "rb") as f:
                data = f.read()
            c.write(f"static const uint8_t {name}_map[] = {{\n")
            for i in range(0, len(data), 16):
                row = ", ".join(f"0x{b:02x}" for b in data[i:i + 16])
                c.write(f"    {row},\n")
            c.write("};\n\n")
            c.write(f"const lv_image_dsc_t gif_{name} = {{\n")
            c.write("    .header = { .cf = LV_COLOR_FORMAT_RAW, .w = 0, .h = 0 },\n")
            c.write(f"    .data_size = sizeof({name}_map),\n")
            c.write(f"    .data = {name}_map,\n")
            c.write("};\n\n")

    with open(out_h, "w") as h:
        h.write("#pragma once\n#include \"lvgl.h\"\n\n")
        for name, _ in items:
            h.write(f"extern const lv_image_dsc_t gif_{name};\n")


if __name__ == "__main__":
    main()

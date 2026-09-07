#!/usr/bin/env python3
"""
Converte PNG(s) com transparencia em header(s) C p/ o dashboard.

  # um arquivo:
  python3 tools/img2sprite.py IMAGEM.png [NOME] [--h 60] [--out src]

  # uma pasta inteira (todos os *.png):
  python3 tools/img2sprite.py PASTA/ [--h 60] [--out src/sprites]

Para cada imagem gera <out>/<nome>.h com:
  - <nome>_data[]  : RGB565, um uint16 por pixel
  - <nome>_mask[]  : 1 bit por pixel (1 = desenhar, 0 = transparente)
  - <nome>_W / <nome>_H
No modo pasta tambem gera <out>/sprites.h com um #include de todos.

Downscale = NEAREST (mantem o pixel-art quadradinho). Use SO com imagens
que voce tem direito de usar.
"""
import os
import re
import sys
from PIL import Image


def ident(stem: str) -> str:
    s = re.sub(r"[^0-9a-zA-Z_]", "_", stem)
    if not s or s[0].isdigit():
        s = "s_" + s
    return s


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def arr(vals, fmt, per):
    return ",\n  ".join(", ".join(fmt % v for v in vals[i:i + per])
                        for i in range(0, len(vals), per))


def convert(path: str, name: str, target_h: int, out_dir: str) -> str:
    im = Image.open(path).convert("RGBA")
    bbox = im.split()[3].getbbox()
    if bbox:
        im = im.crop(bbox)
    w0, h0 = im.size
    tw = max(1, round(target_h * w0 / h0))
    im = im.resize((tw, target_h), Image.NEAREST)
    W, H = im.size
    px = im.load()

    data, mask = [], []
    for y in range(H):
        row = []
        for x in range(W):
            r, g, b, a = px[x, y]
            on = 1 if a >= 128 else 0
            row.append(on)
            data.append(rgb565(r, g, b) if on else 0)
        for b0 in range((W + 7) // 8):
            byte = 0
            for k in range(8):
                xx = b0 * 8 + k
                if xx < W and row[xx]:
                    byte |= (0x80 >> k)
            mask.append(byte)

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, name + ".h")
    with open(dst, "w") as fh:
        fh.write(f"""#pragma once
// Gerado por tools/img2sprite.py a partir de {os.path.basename(path)}
// {W}x{H}. Use apenas com imagem que voce tem direito de usar.

static const uint16_t {name}_data[] PROGMEM = {{
  {arr(data, '0x%04X', 12)}
}};
static const uint8_t {name}_mask[] PROGMEM = {{
  {arr(mask, '0x%02X', 16)}
}};
#define {name}_W {W}
#define {name}_H {H}
""")
    print(f"  {os.path.basename(path):<28} -> {dst}  ({W}x{H}, "
          f"{len(data) * 2 + len(mask)} bytes)")
    return name


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: (a.split("=")[1] if "=" in a else True)
            for a in argv[1:] if a.startswith("--")}
    if not args:
        print(__doc__)
        return 1

    target_h = int(opts.get("--h", 60))
    src = args[0]

    if os.path.isdir(src):
        _o = opts.get("--out"); out_dir = _o if isinstance(_o, str) else "src/sprites"
        pngs = sorted(f for f in os.listdir(src) if f.lower().endswith(".png"))
        if not pngs:
            print("nenhum .png em", src)
            return 1
        print(f"convertendo {len(pngs)} imagens de {src} -> {out_dir}/ (altura {target_h}px)")
        names = [convert(os.path.join(src, f), ident(os.path.splitext(f)[0]),
                         target_h, out_dir) for f in pngs]
        with open(os.path.join(out_dir, "sprites.h"), "w") as fh:
            fh.write("#pragma once\n// indice gerado por img2sprite.py\n")
            for n in names:
                fh.write(f'#include "{n}.h"\n')
        print(f"indice: {out_dir}/sprites.h  ({len(names)} sprites)")
    else:
        name = ident(args[1]) if len(args) > 1 else ident(os.path.splitext(os.path.basename(src))[0])
        _o = opts.get("--out"); out_dir = _o if isinstance(_o, str) else "src"
        print(f"altura {target_h}px -> {out_dir}/")
        n = convert(src, name, target_h, out_dir)
        print("--- preview ---")
        im = Image.open(src).convert("RGBA")
        bbox = im.split()[3].getbbox()
        if bbox:
            im = im.crop(bbox)
        im = im.resize((max(1, round(target_h * im.width / im.height)), target_h), Image.NEAREST)
        p = im.load()
        for y in range(0, im.height, max(1, im.height // 40)):
            print("".join("#" if p[x, y][3] >= 128 else " " for x in range(im.width)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

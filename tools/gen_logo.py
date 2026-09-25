"""Generates the EchoXR logo: installer/logo/echoxr.svg, echoxr.png (512 px) and
echoxr.ico (16-256 px). An "E" sending out two echo arcs, on a blue-violet squircle.

The same geometry is drawn at runtime by DrawLogo() in installer/setup.cpp; keep the
two in step. Coordinates are on a 256-unit canvas.

    python tools/gen_logo.py
"""
import math
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(__file__), "..", "installer", "logo")

BG_A, BG_B = (0x4F, 0x7B, 0xFF), (0x9A, 0x5C, 0xFF)   # top-left -> bottom-right
RADIUS = 60
BARS = [  # x0, y0, x1, y1 (rounded ends)
    (44, 62, 76, 194),     # spine
    (44, 62, 136, 94),     # top
    (44, 112, 118, 144),   # middle
    (44, 162, 136, 194),   # bottom
]
ARC_CX, ARC_CY = 124, 128
ARCS = [(52, 1.0), (86, 0.55)]   # radius, opacity
ARC_W, ARC_SPAN = 20, 48         # stroke width, half-angle in degrees


def svg():
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">',
        '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#%02X%02X%02X"/><stop offset="1" stop-color="#%02X%02X%02X"/>'
        '</linearGradient></defs>' % (BG_A + BG_B),
        '<rect width="256" height="256" rx="%d" fill="url(#g)"/>' % RADIUS,
    ]
    for x0, y0, x1, y1 in BARS:
        r = min(x1 - x0, y1 - y0) / 2
        parts.append('<rect x="%d" y="%d" width="%d" height="%d" rx="%g" fill="#fff"/>' % (x0, y0, x1 - x0, y1 - y0, r))
    for rad, op in ARCS:
        a = math.radians(ARC_SPAN)
        sx, sy = ARC_CX + rad * math.cos(a), ARC_CY - rad * math.sin(a)
        ex, ey = ARC_CX + rad * math.cos(a), ARC_CY + rad * math.sin(a)
        parts.append('<path d="M%.2f %.2f A%d %d 0 0 1 %.2f %.2f" fill="none" stroke="#fff" stroke-opacity="%g" '
                     'stroke-width="%d" stroke-linecap="round"/>' % (sx, sy, rad, rad, ex, ey, op, ARC_W))
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def render(size):
    ss = 4                                  # supersample, then downscale
    n = size * ss
    k = n / 256.0
    # gradient squircle
    g = Image.new("RGBA", (64, 64))
    px = g.load()
    for y in range(64):
        for x in range(64):
            t = (x + y) / 126.0
            px[x, y] = tuple(int(BG_A[i] + (BG_B[i] - BG_A[i]) * t) for i in range(3)) + (255,)
    grad = g.resize((n, n), Image.BILINEAR)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, n - 1, n - 1), radius=RADIUS * k, fill=255)
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    # white mark on its own layer so arc opacity composites correctly
    mark = Image.new("RGBA", (n, n), (255, 255, 255, 0))
    d = ImageDraw.Draw(mark)
    for x0, y0, x1, y1 in BARS:
        r = min(x1 - x0, y1 - y0) / 2 * k
        d.rounded_rectangle((x0 * k, y0 * k, x1 * k, y1 * k), radius=r, fill=(255, 255, 255, 255))
    for rad, op in ARCS:
        layer = Image.new("RGBA", (n, n), (255, 255, 255, 0))
        ld = ImageDraw.Draw(layer)
        ro = rad + ARC_W / 2                  # PIL strokes inward from the box; centre it on rad
        box = ((ARC_CX - ro) * k, (ARC_CY - ro) * k, (ARC_CX + ro) * k, (ARC_CY + ro) * k)
        ld.arc(box, -ARC_SPAN, ARC_SPAN, fill=(255, 255, 255, 255), width=int(ARC_W * k))
        for ang in (-ARC_SPAN, ARC_SPAN):     # round caps
            a = math.radians(ang)
            cx, cy = (ARC_CX + rad * math.cos(a)) * k, (ARC_CY + rad * math.sin(a)) * k
            cr = ARC_W / 2 * k
            ld.ellipse((cx - cr, cy - cr, cx + cr, cy + cr), fill=(255, 255, 255, 255))
        alpha = layer.getchannel("A").point(lambda v: int(v * op))
        layer.putalpha(alpha)
        mark = Image.alpha_composite(mark, layer)
    img = Image.alpha_composite(img, mark)
    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "echoxr.svg"), "w", encoding="utf-8") as f:
        f.write(svg())
    render(512).save(os.path.join(OUT, "echoxr.png"))
    big = render(256)
    big.save(os.path.join(OUT, "echoxr.ico"), sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
    print("wrote", os.path.abspath(OUT))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate the Mothlight monogram variants (SVG + PNG) and a comparison sheet.
   python3 art/make_monograms.py      (needs rsvg-convert and Pillow)"""
import math, os, subprocess, random
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "monogram")
os.makedirs(OUT, exist_ok=True)
CREAM, ORANGE, GOLD, INK = "#F0EDE6", "#F58C1E", "#FFD08A", "#141416"

def iris(cx, cy, r, blades=6, fill=ORANGE, ink=INK, hole=0.42, ring=None, rot=-90, lw=None):
    """a small lens iris: disc, polygonal hole, blade edges"""
    lw = lw or max(1.2, r * 0.16)
    h = r * hole
    pts = [(cx + h * math.cos(math.radians(rot + i * 360 / blades)),
            cy + h * math.sin(math.radians(rot + i * 360 / blades))) for i in range(blades)]
    s = f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{fill}"/>'
    for i, (x, y) in enumerate(pts):
        a = math.radians(rot + i * 360 / blades + 60)       # blade edge leaves the hole tangentially
        x2, y2 = x + r * 1.1 * math.cos(a), y + r * 1.1 * math.sin(a)
        s += f'<line x1="{x:.1f}" y1="{y:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{ink}" stroke-width="{lw:.1f}" stroke-linecap="round"/>'
    s += f'<polygon points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in pts)}" fill="{ink}"/>'
    if ring:
        s += f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{ring}" stroke-width="{lw * 0.8:.1f}"/>'
    # clip the blade lines to the disc by drawing a ring of background over the overshoot
    return f'<g clip-path="url(#ic{int(cx)}{int(cy)}{int(r)})">{s}</g>', \
           f'<clipPath id="ic{int(cx)}{int(cy)}{int(r)}"><circle cx="{cx}" cy="{cy}" r="{r + (lw if ring else 0)}"/></clipPath>'

def wings(color=CREAM, inner=None, style="solid", sw=7):
    left = "M38 156 L38 70 L94 128 L94 156 Z"
    right = "M162 156 L162 70 L106 128 L106 156 Z"
    if style == "outline":
        return (f'<path d="{left}" fill="none" stroke="{color}" stroke-width="{sw}" stroke-linejoin="round"/>'
                f'<path d="{right}" fill="none" stroke="{color}" stroke-width="{sw}" stroke-linejoin="round"/>')
    s = f'<path d="{left}" fill="{color}"/><path d="{right}" fill="{color}"/>'
    if inner:
        s += (f'<path d="M38 70 L94 128 L94 112 L46 62 Z" fill="{inner}"/>'
              f'<path d="M162 70 L106 128 L106 112 L154 62 Z" fill="{inner}"/>')
    return s

def glow(cx, cy, r, color=ORANGE, op=0.8, gid="g"):
    return (f'<radialGradient id="{gid}" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="{color}" stop-opacity="{op}"/>'
            f'<stop offset="1" stop-color="{color}" stop-opacity="0"/></radialGradient>',
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="url(#{gid})"/>')

def stars(n=40, seed=3):
    rnd = random.Random(seed)
    return "".join(f'<circle cx="{rnd.uniform(8,192):.1f}" cy="{rnd.uniform(8,120):.1f}" r="{rnd.choice([0.6,0.8,1.1])}" '
                   f'fill="#FFFFFF" opacity="{rnd.uniform(0.25,0.8):.2f}"/>' for _ in range(n))

def rays(cx, cy, n=12, r0=16, r1=48, color=GOLD, op=0.35):
    s = ""
    for i in range(n):
        a = math.radians(i * 360 / n)
        s += (f'<line x1="{cx + r0 * math.cos(a):.1f}" y1="{cy + r0 * math.sin(a):.1f}" x2="{cx + r1 * math.cos(a):.1f}" '
              f'y2="{cy + r1 * math.sin(a):.1f}" stroke="{color}" stroke-width="2" stroke-linecap="round" opacity="{op}"/>')
    return s

def svg(defs, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">'
            f'<defs>{defs}</defs><clipPath id="tile"><rect width="200" height="200" rx="44"/></clipPath>'
            f'<g clip-path="url(#tile)">{body}</g></svg>')

V = []

# M1 Iris dot: the light becomes an aperture
g_d, g_b = glow(100, 54, 38, gid="g1")
i_b, i_d = iris(100, 54, 15, ring=CREAM)
V.append(("M1", "Iris", "The light is a 6-blade aperture.", svg(g_d + i_d, f'<rect width="200" height="200" fill="{INK}"/>{g_b}{wings(CREAM, "#8C8A86")}{i_b}')))

# M2 Iris + rays on a warm radial background
g_d, g_b = glow(100, 54, 70, color="#F58C1E", op=0.55, gid="g2")
i_b, i_d = iris(100, 54, 14, ring=GOLD)
bg = ('<radialGradient id="bg2" cx="50%" cy="30%" r="80%"><stop offset="0" stop-color="#3A2210"/><stop offset="1" stop-color="#0E0E10"/></radialGradient>')
V.append(("M2", "Lantern rays", "Warm halo, light rays, iris core.", svg(g_d + i_d + bg, f'<rect width="200" height="200" fill="url(#bg2)"/>{g_b}{rays(100,54)}{wings(CREAM, "#B9A58A")}{i_b}')))

# M3 Night sky: stars, moon-like iris, wings catching the light
bg = ('<linearGradient id="bg3" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0B1026"/><stop offset="0.65" stop-color="#1B1540"/><stop offset="1" stop-color="#2A1630"/></linearGradient>'
      '<linearGradient id="w3" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFE7C2"/><stop offset="1" stop-color="#E9E2F5"/></linearGradient>')
g_d, g_b = glow(100, 54, 44, color="#FFB347", op=0.7, gid="g3")
i_b, i_d = iris(100, 54, 13, fill="#FFB347", ring=None)
V.append(("M3", "Night flight", "Starfield sky, gradient wings.", svg(bg + g_d + i_d, f'<rect width="200" height="200" fill="url(#bg3)"/>{stars()}{g_b}{wings("url(#w3)", "#9C93B8")}{i_b}')))

# M4 Lens barrel: concentric rings and focus-scale ticks behind the M
ticks = "".join(f'<line x1="{100 + 86 * math.cos(math.radians(a)):.1f}" y1="{108 + 86 * math.sin(math.radians(a)):.1f}" '
                f'x2="{100 + (80 if a % 30 else 74) * math.cos(math.radians(a)):.1f}" y2="{108 + (80 if a % 30 else 74) * math.sin(math.radians(a)):.1f}" '
                f'stroke="#5A5A60" stroke-width="2"/>' for a in range(-180, 1, 10))
rings = "".join(f'<circle cx="100" cy="108" r="{r}" fill="none" stroke="#2C2C31" stroke-width="{w}"/>' for r, w in ((92, 6), (70, 2), (58, 1)))
i_b, i_d = iris(100, 52, 14, ring=CREAM)
V.append(("M4", "Lens barrel", "Focus-ring ticks and lens rings.", svg(i_d, f'<rect width="200" height="200" fill="#17171A"/>{rings}{ticks}{wings(CREAM, "#7E7C79")}{i_b}')))

# M5 Inverted: orange tile, ink monogram, cream iris
i_b, i_d = iris(100, 54, 15, fill=CREAM, ink=ORANGE, ring=INK)
V.append(("M5", "Signal orange", "Inverted: loud, poster-like.", svg(i_d, f'<rect width="200" height="200" fill="{ORANGE}"/>{wings(INK, "#5C2E08")}{i_b}')))

# M6 Line art: outline M, outline iris, for print and stickers
i_b, i_d = iris(100, 54, 15, fill="none", ink=CREAM, ring=CREAM, lw=2.4)
V.append(("M6", "Line", "Outline only: engraving, stickers.", svg(i_d, f'<rect width="200" height="200" fill="{INK}"/>{wings(CREAM, style="outline")}{i_b}')))

# M7 Film strip: sprocket holes top and bottom, warm iris
holes = "".join(f'<rect x="{x}" y="{y}" width="12" height="9" rx="2" fill="#26262B"/>' for x in range(10, 200, 22) for y in (10, 181))
bg = '<linearGradient id="bg7" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#1D1D22"/><stop offset="1" stop-color="#101012"/></linearGradient>'
g_d, g_b = glow(100, 56, 36, gid="g7")
i_b, i_d = iris(100, 56, 14, ring=CREAM)
V.append(("M7", "Film strip", "Sprocket edges: the cinema angle.", svg(bg + g_d + i_d, f'<rect width="200" height="200" fill="url(#bg7)"/>{holes}{g_b}{wings(CREAM, "#8C8A86")}{i_b}')))

# M8 Dusk: sunset gradient (like the UI previews), dark monogram silhouette, glowing iris
bg = ('<linearGradient id="bg8" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#F6A15B"/><stop offset="0.55" stop-color="#B9578A"/><stop offset="1" stop-color="#2B1B3A"/></linearGradient>')
g_d, g_b = glow(100, 54, 40, color="#FFF0C8", op=0.85, gid="g8")
i_b, i_d = iris(100, 54, 14, fill="#FFE2A8", ink="#2B1B3A", ring="#FFF6E0")
V.append(("M8", "Dusk", "Sunset sky, silhouette wings.", svg(bg + g_d + i_d, f'<rect width="200" height="200" fill="url(#bg8)"/>{g_b}{wings("#1A1222", "#3D2A4A")}{i_b}')))

for key, name, _, s in V:
    base = os.path.join(OUT, f"{key.lower()}-{name.lower().replace(' ', '-')}")
    open(base + ".svg", "w").write(s)
    subprocess.run(["rsvg-convert", "-w", "512", "-h", "512", base + ".svg", "-o", base + ".png"], check=True)

# comparison sheet: big tile, plus 48/32/16 px, 4 x 2
B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"; R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
cw, chh = 380, 470
sheet = Image.new("RGB", (40 + 4 * cw, 110 + 2 * chh), (22, 22, 25))
d = ImageDraw.Draw(sheet)
d.text((40, 32), "Mothlight monogram - variants", fill=(240, 237, 230), font=ImageFont.truetype(B, 34))
for i, (key, name, blurb, _) in enumerate(V):
    x = 40 + (i % 4) * cw; y = 110 + (i // 4) * chh
    png = Image.open(os.path.join(OUT, f"{key.lower()}-{name.lower().replace(' ', '-')}.png")).convert("RGBA")
    big = png.resize((280, 280), Image.LANCZOS); sheet.paste(big, (x, y), big)
    d.text((x, y + 292), f"{key}  {name}", fill=(245, 140, 30), font=ImageFont.truetype(B, 22))
    d.text((x, y + 322), blurb, fill=(185, 185, 185), font=ImageFont.truetype(R, 17))
    for j, s in enumerate((48, 32, 16)):
        t = png.resize((s, s), Image.LANCZOS); sheet.paste(t, (x + [0, 64, 112][j], y + 360 + (48 - s) // 2), t)
    d.text((x + 140, y + 374), "48 / 32 / 16", fill=(110, 110, 110), font=ImageFont.truetype(R, 15))
sheet.save(os.path.join(os.path.dirname(__file__), "monograms.png"))
print(len(V), "variants")

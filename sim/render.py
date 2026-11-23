#!/usr/bin/env python3
"""Render the simulator's output: an animated GIF per theme, stills, and a theme gallery.

    python3 sim/render.py <preview dir> <out dir>      (expects <preview dir>/frames0..5)
"""
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

THEMES = ["Mothlight", "Stock", "Studio", "Amber", "Signal", "Material", "Soft", "Glass", "Daylight", "Night", "Mono"]
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


def frames_of(t):
    return [Image.open(p).convert("RGB") for p in sorted(glob.glob(os.path.join(src, f"frames{t}", "frame*.ppm")))]


def at(frames, ms):
    return frames[min(len(frames) - 1, ms // 33)]


def gif(frames, path, size=(720, 480)):
    small = [f.resize(size, Image.LANCZOS).quantize(colors=128, method=Image.Quantize.MEDIANCUT) for f in frames]
    small[0].save(path, save_all=True, append_images=small[1:], duration=33, loop=0, optimize=True)


font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
label_font = ImageFont.truetype(font_path, 22) if os.path.exists(font_path) else ImageFont.load_default()

stills = []
for t, name in enumerate(THEMES):
    fr = frames_of(t)
    if not fr:
        continue
    if t == 0:
        gif(fr, os.path.join(out, "panel.gif"))
        for still, ms in (("open", 800), ("scrolled", 3300), ("applying", 5700), ("done", 6600)):
            at(fr, ms).save(os.path.join(out, f"panel-{still}.png"))
    gif(fr, os.path.join(out, f"theme-{name.lower()}.gif"))
    s = at(fr, 3300)
    s.save(os.path.join(out, f"theme-{name.lower()}.png"))
    stills.append((name, s))

# 3 x 2 gallery with labels
w, h = 720, 480
pad, lab = 24, 40
cols = 4
rows = (len(stills) + cols - 1) // cols
g = Image.new("RGB", (cols * w + (cols + 1) * pad, rows * (h + lab) + (rows + 1) * pad), (18, 18, 20))
d = ImageDraw.Draw(g)
for i, (name, im) in enumerate(stills):
    x = pad + (i % cols) * (w + pad)
    y = pad + (i // cols) * (h + lab + pad)
    d.text((x, y), f"{i + 1}. {name}", fill=(235, 235, 235), font=label_font)
    g.paste(im, (x, y + lab))
g.save(os.path.join(out, "themes.png"))
print(f"{len(stills)} themes -> {out}/themes.png, theme-*.gif, panel.gif")

#!/usr/bin/env python3
"""e2e frames -> full-resolution MP4 + GIF, and a labelled storyboard sheet.
   python3 sim/render_e2e.py <frames dir> <out dir>"""
import glob, os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1], sys.argv[2]
frames = sorted(glob.glob(os.path.join(src, "frame*.ppm")))
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "25", "-i", os.path.join(src, "frame%04d.ppm"),
                "-c:v", "libx264", "-crf", "16", "-pix_fmt", "yuv420p", "-preset", "veryfast",
                os.path.join(out, "mothlight-e2e.mp4")], check=True)
# GIF at the screen's own 720 x 480, 12.5 fps (every 2nd frame) to stay shareable
ims = [Image.open(p).convert("RGB") for p in frames[::2]]
q = [im.quantize(colors=96, method=Image.Quantize.MEDIANCUT) for im in ims]
q[0].save(os.path.join(out, "mothlight-e2e.gif"), save_all=True, append_images=q[1:], duration=80, loop=0, optimize=True)

# storyboard: key moments
B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
keys = [(800, "1  LiveView: ML bars + Mothlight budget (green)"), (1320, "2  INFO: iris opens"),
        (2000, "3  Panel: list + detail pane"), (3500, "4  3K Scope: crop box, BURST"),
        (4500, "5  RIGHT: Stock theme, live"), (5500, "6  Studio theme"),
        (8300, "7  SET: applying (spinner)"), (9300, "8  Ready, auto-close"),
        (10400, "9  LiveView: budget now red"), (12500, "10 Recording: red tally"),
        (15600, "11 Stop: learned compression"), (18000, "12 Back to LiveView")]
W, H, pad, lab = 720, 480, 16, 34
cols = 3
rows = (len(keys) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + lab + pad) + pad), (16, 16, 18))
d = ImageDraw.Draw(sheet)
f = ImageFont.truetype(B, 21)
for i, (ms, label) in enumerate(keys):
    im = Image.open(frames[min(len(frames) - 1, ms // 40)]).convert("RGB")
    x = pad + (i % cols) * (W + pad); y = pad + (i // cols) * (H + lab + pad)
    d.text((x, y + 4), label, fill=(240, 237, 230), font=f)
    sheet.paste(im, (x, y + lab))
sheet.save(os.path.join(out, "mothlight-storyboard.png"))
print(len(frames), "frames ->", out)

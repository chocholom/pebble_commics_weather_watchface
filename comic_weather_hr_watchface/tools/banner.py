"""
Store marketing banner (720x320) for the Rebble appstore listing.
Reuses the comic art helpers from preview.py: red action rays, a hand-drawn
starburst with the title, and two tilted device screenshots from store_assets/.

    python3 tools/banner.py   ->  store_assets/banner_720x320.png
"""
from pathlib import Path

from PIL import Image, ImageDraw

import preview as P
from preview import C, sc, xy, wpoly, wline, benday, burst_pts, sparkle, txt, lilita

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'store_assets'
BW, BH, S = 720, 320, P.S

F_TITLE = lilita(64)
F_SUB = lilita(22)


def rays(d, cx, cy, n=26, rot=6):
    R = 1200
    import math
    for i in range(n):
        a0 = rot + i * 360.0 / n
        a1 = a0 + 360.0 / n * 0.5
        pts = [(cx, cy)]
        for a in (a0, a1):
            rad = math.radians(a)
            pts.append((cx + math.cos(rad) * R, cy + math.sin(rad) * R))
        d.polygon([xy(p) for p in pts], fill=C['red_ray'])


def screenshot_card(path, scale, tilt):
    """Device screenshot in a white comic frame with ink border, tilted."""
    shot = Image.open(path).convert('RGB')
    w, h = int(shot.width * scale), int(shot.height * scale)
    shot = shot.resize((w * S, h * S), Image.Resampling.NEAREST)
    pad = sc(7)
    card = Image.new('RGBA', (shot.width + pad * 2, shot.height + pad * 2), (0, 0, 0, 0))
    cd = ImageDraw.Draw(card)
    cw, ch = card.width / S, card.height / S
    wpoly(cd, [(1, 1), (cw - 1, 1), (cw - 1, ch - 1), (1, ch - 1)],
          C['white'] + (255,), C['ink'] + (255,), 2.6, amp=0.5, seed=201)
    card.paste(shot, (pad, pad))
    return card.rotate(tilt, resample=Image.Resampling.BICUBIC, expand=True)


def main():
    im = Image.new('RGB', (BW * S, BH * S), C['red'])
    d = ImageDraw.Draw(im)
    rays(d, 235, 165)
    benday(d, (10, 10, 300, 90), C['red_ray'], 16, 1.6)
    benday(d, (380, 240, 700, 312), C['red_ray'], 16, 1.6)

    # tilted device screenshots, comic-panel style (back one first)
    back = screenshot_card(ASSETS / '4_storm.png', 0.95, -8)
    front = screenshot_card(ASSETS / '1_clear.png', 1.0, 3.5)
    im.paste(back, (sc(428), sc(24)), back)
    im.paste(front, (sc(488), sc(30)), front)

    # title burst
    bp = burst_pts(235, 150, 235, 128, 158, 82, n=12, seed=15)
    d.polygon([xy((x + 5, y + 7)) for (x, y) in bp], fill=C['red_dk'])
    wpoly(d, bp, C['white'], C['ink'], 3.2, amp=0.8, seed=202)

    # title: gold bubble lettering with ink outline + shadow
    for i, (line, ty) in enumerate((('COMIC', 66), ('WEATHER', 138))):
        txt(d, (237 + 2.5, ty + 3), line, F_TITLE, C['gold_sh'], anchor='ma',
            stroke=2.2, stroke_fill=C['gold_sh'])
        txt(d, (237, ty), line, F_TITLE, C['gold'], anchor='ma',
            stroke=2.2, stroke_fill=C['ink'])

    # subtitle caption box
    subs = 'FORECAST + STEPS + HR'
    bb = d.textbbox((0, 0), subs, font=F_SUB)
    sw = (bb[2] - bb[0]) / S + 22
    box = [(237 - sw / 2, 216), (237 + sw / 2, 216), (237 + sw / 2, 246), (237 - sw / 2, 246)]
    wpoly(d, box, C['ink'], C['white'], 1.6, amp=0.5, seed=203)
    txt(d, (237, 231), subs, F_SUB, C['white'], anchor='mm')

    # comic garnish
    P._sun(d, 46, 42, 24, nrays=10, seed=205, lw=2.2)
    sparkle(d, 412, 52, 11, C['yellow'], seed=207)
    sparkle(d, 60, 262, 9, C['white'], seed=208)
    sparkle(d, 350, 290, 7, C['yellow'], seed=209)
    sparkle(d, 438, 300, 6, C['white'], seed=210)

    out = im.resize((BW, BH), Image.Resampling.LANCZOS)
    out.save(ASSETS / 'banner_720x320.png')
    print('wrote', ASSETS / 'banner_720x320.png')


if __name__ == '__main__':
    main()

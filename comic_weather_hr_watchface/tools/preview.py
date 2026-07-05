"""
Comic Weather HR - asset & preview generator (v2 "hand-drawn comic page" redesign).

Generates the static comic art (base_layout), the six weather scene panels,
the small RGBA forecast icons, and a faithful mock of the on-watch render.
The mock replicates exactly what src/c/main.c draws on top of the static art.

Design language (modeled on the "Comic Drop" watchface):
  - full-bleed comic panels separated by white gutters and wobbly ink borders
  - radial "action ray" backgrounds (purple / red / orange)
  - a huge irregular starburst holding the time in gold bubble numerals
  - a tilted spiral-bound tear-off calendar
  - comic caption chips for steps / heart rate / battery (no sterile status bar)
"""
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from pathlib import Path
import math
import random

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / 'resources' / 'images'
PREV = ROOT / 'preview'
FONTS = ROOT / 'tools' / 'fonts'
RES.mkdir(parents=True, exist_ok=True)
PREV.mkdir(parents=True, exist_ok=True)

W, H, S = 200, 228, 4

# Pebble Time 2 has a 64-color palette: each channel is one of {0, 85, 170, 255}.
# Every colour below is authored on that grid so it survives on-device unchanged.
C = {
    'ink': (0, 0, 0), 'paper': (255, 255, 255), 'white': (255, 255, 255),
    'purple': (85, 0, 170), 'purple_ray': (85, 0, 255),
    'red': (255, 0, 0), 'red_ray': (170, 0, 0), 'red_dk': (85, 0, 0),
    'gold': (255, 170, 0), 'gold_sh': (170, 85, 0),
    'yellow': (255, 255, 0), 'orange': (255, 170, 0), 'orange_ray': (255, 85, 0),
    'cream': (255, 255, 170), 'dot_pink': (255, 170, 170),
    'blue': (0, 85, 255), 'blue_dk': (0, 0, 170), 'blue_sh': (0, 85, 170),
    'sky_clear': (85, 170, 255), 'sky_cloud': (170, 170, 255),
    'sky_rain': (85, 85, 170), 'sky_storm': (85, 85, 85),
    'sky_fog': (170, 170, 170), 'sky_snow': (170, 170, 255),
    'grass': (85, 255, 0), 'grass_dk': (0, 170, 0), 'grass_gloom': (0, 85, 0),
    'cloud_grey': (170, 170, 170), 'steel': (170, 170, 170),
    'batt_green': (0, 255, 0),
}

_PEBBLE_LUT = [min(255, int(round(i / 85.0)) * 85) for i in range(256)]
def pebblize(im):
    # Sharpen the downscaled art before snapping to the 64-colour grid: this
    # steepens anti-aliased edges so quantisation produces crisp 1px ink lines
    # instead of speckled halos at native resolution.
    im = im.convert('RGB').filter(ImageFilter.UnsharpMask(radius=1.1, percent=170, threshold=2))
    return im.point(_PEBBLE_LUT * 3)

def art_scale(im, w, h):
    return im.resize((w, h), Image.Resampling.BOX)

def lilita(px):
    return ImageFont.truetype(str(FONTS / 'LilitaOne-Regular.ttf'), int(px * S))

F_TIME, F_BIG, F_MED, F_SMALL = lilita(44), lilita(26), lilita(16), lilita(14)
F_CAP12, F_CAP10 = lilita(12.5), lilita(10.5)

def sc(v): return int(round(v * S))
def xy(p): return (sc(p[0]), sc(p[1]))
def box(b): return tuple(sc(x) for x in b)

# ---------------------------------------------------------------- hand-drawn ink
# Low-frequency, low-amplitude wobble: reads as confident hand inking.
# (High-frequency jitter reads as pixel noise at 200px native resolution.)
def wpts(pts, amp=0.45, seg=7.0, seed=1, closed=False):
    """Subdivide a polyline and jitter it perpendicular: wobbly hand-drawn ink."""
    rnd = random.Random(seed)
    src = list(pts) + ([pts[0]] if closed else [])
    out = []
    for i in range(len(src) - 1):
        (x0, y0), (x1, y1) = src[i], src[i + 1]
        dist = math.hypot(x1 - x0, y1 - y0) or 1.0
        n = max(1, int(dist / seg))
        nx, ny = -(y1 - y0) / dist, (x1 - x0) / dist
        for j in range(n):
            t = j / n
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            if not (i == 0 and j == 0):
                off = rnd.uniform(-amp, amp)
                x, y = x + nx * off, y + ny * off
            out.append((x, y))
    out.append(src[-1])
    return out

def wline(d, pts, fill, width=1.4, amp=0.45, seed=1):
    d.line([xy(p) for p in wpts(pts, amp=amp, seed=seed)], fill=fill, width=sc(width), joint='curve')

def wpoly(d, pts, fill=None, outline=None, width=1.6, amp=0.45, seed=1):
    if fill is not None:
        d.polygon([xy(p) for p in pts], fill=fill)
    if outline is not None:
        d.line([xy(p) for p in wpts(pts, amp=amp, seed=seed, closed=True)],
               fill=outline, width=sc(width), joint='curve')

def ellipse(d, b, fill=None, outline=None, width=1):
    d.ellipse(box(b), fill=fill, outline=outline, width=sc(width))

def rect(d, b, fill=None, outline=None, width=1):
    d.rectangle(box(b), fill=fill, outline=outline, width=sc(width))

def txt(d, p, t, f, fill, anchor='la', stroke=0, stroke_fill=None):
    d.text(xy(p), t, font=f, fill=fill, anchor=anchor,
           stroke_width=sc(stroke), stroke_fill=stroke_fill or fill)

def benday(d, b, dot, step=8, r=1.1):
    x0, y0, x1, y1 = [int(v) for v in b]
    for iy, y in enumerate(range(y0, y1, step)):
        off = step // 2 if iy % 2 else 0
        for x in range(x0 + off, x1, step):
            ellipse(d, (x - r, y - r, x + r, y + r), fill=dot)

def rays_panel(im, poly, center, base, ray, n=16, rot=8.0):
    """Fill a panel polygon with a radial sunburst (alternating wedges)."""
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).polygon([xy(p) for p in poly], fill=255)
    layer = Image.new('RGB', im.size, base)
    ld = ImageDraw.Draw(layer)
    cx, cy = center
    R = 400
    for i in range(n):
        a0 = rot + i * 360.0 / n
        a1 = a0 + 360.0 / n * 0.5
        p = [(cx, cy)]
        for a in (a0, a1):
            rad = math.radians(a)
            p.append((cx + math.cos(rad) * R, cy + math.sin(rad) * R))
        ld.polygon([xy(q) for q in p], fill=ray)
    im.paste(layer, (0, 0), mask)

def burst_pts(cx, cy, rxo, ryo, rxi, ryi, n=13, seed=7, rot=-90):
    """Irregular hand-drawn starburst outline points."""
    rnd = random.Random(seed)
    pts = []
    for i in range(2 * n):
        a = rot + i * 180.0 / n + rnd.uniform(-3.5, 3.5)
        if i % 2 == 0:
            rx, ry = rxo * rnd.uniform(0.78, 1.12), ryo * rnd.uniform(0.80, 1.15)
        else:
            rx, ry = rxi * rnd.uniform(0.86, 1.08), ryi * rnd.uniform(0.86, 1.08)
        rad = math.radians(a)
        pts.append((cx + math.cos(rad) * rx, cy + math.sin(rad) * ry))
    return pts

def sparkle(d, cx, cy, r, fill, seed=3):
    pts = []
    for i in range(8):
        ang = math.radians(-90 + i * 45)
        rr = r if i % 2 == 0 else r * 0.38
        pts.append((cx + math.cos(ang) * rr, cy + math.sin(ang) * rr))
    wpoly(d, pts, fill, C['ink'], 1.0, amp=0.3, seed=seed)

def caption(im, cx, cy, w, h, text, f, fill, tilt=-3.0, seed=5, txt_fill=None):
    """A tilted comic caption box with hand-lettered text, pasted onto im."""
    pad = sc(8)
    lw, lh = sc(w) + pad * 2, sc(h) + pad * 2
    layer = Image.new('RGBA', (lw, lh), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    bx = [(pad / S, pad / S), (pad / S + w, pad / S), (pad / S + w, pad / S + h), (pad / S, pad / S + h)]
    wpoly(ld, bx, fill + (255,), C['ink'] + (255,), 1.8, amp=0.5, seed=seed)
    ld.text((lw / 2, lh / 2), text, font=f, fill=(txt_fill or C['white']) + (255,),
            anchor='mm', stroke_width=sc(0.9), stroke_fill=C['ink'] + (255,))
    layer = layer.rotate(tilt, resample=Image.Resampling.BICUBIC, expand=True)
    im.paste(layer, (sc(cx) - layer.width // 2, sc(cy) - layer.height // 2), layer)

# ---------------------------------------------------------------- scene bits
def _sun(d, cx, cy, r, nrays=9, seed=11, ray_len=None, lw=1.4):
    """Comic sun: cel-shaded disc + confident triangle rays."""
    rl = ray_len if ray_len is not None else r * 0.75
    rnd = random.Random(seed)
    for i in range(nrays):
        a = i * 360.0 / nrays + rnd.uniform(-4, 4)
        rad = math.radians(a)
        w_half = math.radians(11)
        p0 = (cx + math.cos(rad - w_half) * (r + 1.0), cy + math.sin(rad - w_half) * (r + 1.0))
        p1 = (cx + math.cos(rad + w_half) * (r + 1.0), cy + math.sin(rad + w_half) * (r + 1.0))
        tip = (cx + math.cos(rad) * (r + 1.0 + rl * rnd.uniform(0.9, 1.15)),
               cy + math.sin(rad) * (r + 1.0 + rl * rnd.uniform(0.9, 1.15)))
        wpoly(d, [p0, tip, p1], C['yellow'], C['ink'], lw * 0.85, amp=0.2, seed=seed + i)
    # disc with a gold cel-shadow crescent hugging the bottom-right rim
    ellipse(d, (cx - r, cy - r, cx + r, cy + r), C['yellow'])
    d.arc(box((cx - r, cy - r, cx + r, cy + r)), -10, 120, fill=C['gold'], width=sc(r * 0.34))
    pts = [(cx + math.cos(math.radians(a)) * r, cy + math.sin(math.radians(a)) * r)
           for a in range(0, 360, 20)]
    wpoly(d, pts, None, C['ink'], lw, amp=0.3, seed=seed)

def _cloud(d, x, y, w, h, fill=None, seed=21, lw=1.5, shade=None):
    """Puffy comic cloud: cel-shaded underside + wobbly ink outline."""
    fill = fill or C['white']
    shade = shade or C['steel']
    ellipse(d, (x, y + h * 0.30, x + w * 0.46, y + h * 1.02), fill)
    ellipse(d, (x + w * 0.22, y - h * 0.10, x + w * 0.68, y + h * 0.80), fill)
    ellipse(d, (x + w * 0.46, y + h * 0.18, x + w * 1.00, y + h * 1.02), fill)
    # flat cel shadow along the belly
    d.polygon([xy(p) for p in [(x + w * 0.10, y + h * 0.78), (x + w * 0.90, y + h * 0.78),
                               (x + w * 0.86, y + h * 0.98), (x + w * 0.14, y + h * 0.98)]],
              fill=shade)
    rnd = random.Random(seed)
    arcs = [(x, y + h * 0.30, x + w * 0.46, y + h * 1.02, 110, 320),
            (x + w * 0.22, y - h * 0.10, x + w * 0.68, y + h * 0.80, 160, 380),
            (x + w * 0.46, y + h * 0.18, x + w * 1.00, y + h * 1.02, 220, 70)]
    for (ax0, ay0, ax1, ay1, a0, a1) in arcs:
        d.arc(box((ax0, ay0, ax1, ay1)), a0 + rnd.uniform(-5, 5), a1 + rnd.uniform(-5, 5),
              fill=C['ink'], width=sc(lw))
    wline(d, [(x + w * 0.06, y + h * 0.98), (x + w * 0.94, y + h * 0.98)], C['ink'], lw, 0.3, seed)

def _pine(d, x, base_y, h, seed=31, snow=False):
    """Two-tier comic pine sitting on the ground at (x..x+0.9h, base_y)."""
    fill = C['grass_dk'] if not snow else C['white']
    cx = x + h * 0.45
    rect(d, (cx - 1.2, base_y - 1, cx + 1.2, base_y + 2), (85, 85, 0), C['ink'], 1.0)
    wpoly(d, [(x, base_y), (cx, base_y - h * 0.62), (x + h * 0.9, base_y)],
          fill, C['ink'], 1.2, amp=0.35, seed=seed)
    wpoly(d, [(x + h * 0.14, base_y - h * 0.42), (cx, base_y - h),
              (x + h * 0.76, base_y - h * 0.42)], fill, C['ink'], 1.2, amp=0.35, seed=seed + 1)

def _bolt(d, cx, top, sc_=1.0, seed=41):
    """Fat tapering comic lightning bolt."""
    p = [(cx, top), (cx - 8 * sc_, top + 13 * sc_), (cx - 3 * sc_, top + 13 * sc_),
         (cx - 10 * sc_, top + 27 * sc_), (cx + 3 * sc_, top + 15 * sc_),
         (cx - 1.5 * sc_, top + 15 * sc_), (cx + 6 * sc_, top)]
    wpoly(d, p, C['yellow'], C['ink'], 1.4, amp=0.25, seed=seed)
    # inner highlight stroke
    wline(d, [(cx + 1.5 * sc_, top + 3 * sc_), (cx - 4 * sc_, top + 11 * sc_)],
          C['white'], 1.2 * max(sc_, 0.6), 0.1, seed=seed + 1)

# ---------------------------------------------------------------- scenes
# Scene panel occupies screen rect (84,20)-(200,97). Its left gutter, slanted
# bottom edge (following the page's tilted horizon, see PAGE GEOMETRY) and ink
# frame are baked into the PNG so main.c can blit it as a plain rectangle.
SCENE_W, SCENE_H = 116, 77
SCENE_POLY = [(6, 0), (SCENE_W, 0), (SCENE_W, SCENE_H), (0, SCENE_H - 9)]

def scene(kind):
    im = Image.new('RGB', (SCENE_W * S, SCENE_H * S), C['paper'])
    d = ImageDraw.Draw(im)
    sky = {'clear': C['sky_clear'], 'cloud': C['sky_cloud'], 'rain': C['sky_rain'],
           'snow': C['sky_snow'], 'storm': C['sky_storm'], 'fog': C['sky_fog']}[kind]
    sky_low = {'clear': (170, 255, 255), 'cloud': (170, 255, 255), 'rain': (170, 170, 255),
               'snow': (170, 255, 255), 'storm': (85, 85, 170), 'fog': None}[kind]
    d.polygon([xy(p) for p in SCENE_POLY], fill=sky)
    gy = 50  # horizon

    # posterised sky: a lighter band near the horizon (classic printed-comic sky)
    if sky_low:
        d.polygon([xy(p) for p in [(0, 30), (34, 33), (72, 29), (116, 32),
                                   (116, SCENE_H), (0, SCENE_H - 9)]], fill=sky_low)

    # distant mountain silhouettes for depth
    if kind in ('clear', 'cloud', 'rain', 'storm'):
        mtn = C['blue_dk'] if kind in ('clear', 'cloud') else (85, 85, 170)
        ridge = [(0, gy + 2), (16, 38), (34, gy - 2), (52, 34), (74, gy - 4),
                 (94, 39), (116, gy + 2)]
        d.polygon([xy(p) for p in ridge + [(116, gy + 8), (0, gy + 8)]], fill=mtn)
        wline(d, ridge, C['ink'], 1.3, 0.3, seed=60)

    grass = {'clear': C['grass'], 'cloud': C['grass'], 'rain': C['grass_gloom'],
             'snow': C['white'], 'storm': C['grass_gloom'], 'fog': C['cloud_grey']}[kind]
    grass_sh = {'clear': C['grass_dk'], 'cloud': C['grass_dk'], 'rain': (0, 85, 0),
                'snow': (170, 255, 255), 'storm': (0, 85, 0), 'fog': C['steel']}[kind]

    # ground: rolling hills with a cel-shaded far slope
    hill = [(0, SCENE_H), (0, gy + 4), (20, gy - 2), (52, gy + 5), (86, gy - 1),
            (SCENE_W, gy + 4), (SCENE_W, SCENE_H)]
    d.polygon([xy(p) for p in hill], fill=grass)
    d.polygon([xy(p) for p in [(30, SCENE_H), (52, gy + 5), (86, gy - 1), (112, gy + 8),
                               (116, gy + 8), (116, SCENE_H)]], fill=grass_sh)
    wline(d, [(52, gy + 5), (86, gy - 1), (112, gy + 8)], C['ink'], 1.4, 0.4, seed=62)
    wline(d, hill[1:-1], C['ink'], 1.6, 0.45, seed=61)

    if kind == 'clear':
        _sun(d, 26, 18, 9.5, nrays=10, seed=11, lw=1.5)
        _cloud(d, 52, 30, 24, 8, seed=22)
        for bx, by in ((84, 38), (95, 34)):
            wline(d, [(bx - 3, by + 1.6), (bx, by), (bx + 3, by + 1.6)], C['ink'], 1.2, 0.1, seed=int(bx))
        _pine(d, 8, gy + 12, 13, seed=31)
        _pine(d, 20, gy + 15, 10, seed=32)
        _pine(d, 100, gy + 13, 12, seed=33)
    elif kind == 'cloud':
        _sun(d, 30, 15, 7, nrays=9, seed=12, ray_len=4.5)
        _cloud(d, 12, 12, 34, 12, seed=23)
        _cloud(d, 52, 20, 30, 10, fill=C['cloud_grey'], shade=(85, 85, 85), seed=24)
        _cloud(d, 78, 6, 28, 10, seed=25)
        _pine(d, 10, gy + 12, 12, seed=34)
        _pine(d, 98, gy + 14, 11, seed=35)
    elif kind == 'rain':
        _cloud(d, 8, 8, 40, 13, fill=C['cloud_grey'], shade=(85, 85, 85), seed=26)
        _cloud(d, 44, 16, 34, 11, fill=C['steel'], shade=(85, 85, 85), seed=27)
        rnd = random.Random(71)
        for i in range(9):
            rx = 14 + i * 11 + rnd.uniform(-2, 2)
            ry = 32 + rnd.uniform(0, 6)
            wline(d, [(rx, ry), (rx - 2.5, ry + 8)], C['blue'], 1.7, 0.15, seed=71 + i)
        # splashes on the ground
        for sx in (30, 62, 92):
            wline(d, [(sx - 2, gy + 11), (sx, gy + 9), (sx + 2, gy + 11)], C['sky_cloud'], 1.2, 0.1, seed=sx)
        for px, pw in ((26, 14), (74, 18)):
            ellipse(d, (px, gy + 13, px + pw, gy + 17), C['sky_cloud'], C['ink'], 1.2)
    elif kind == 'snow':
        _cloud(d, 10, 8, 36, 12, seed=28)
        _cloud(d, 60, 14, 30, 10, seed=29)
        rnd = random.Random(81)
        for i in range(16):
            fx = 10 + i * 6.6 + rnd.uniform(-3, 3)
            fy = 28 + rnd.uniform(0, 16)
            r = rnd.uniform(1.1, 1.8)
            ellipse(d, (fx - r, fy - r, fx + r, fy + r), C['white'], C['ink'], 0.9)
        # snow drift contours
        wline(d, [(14, gy + 10), (34, gy + 8), (52, gy + 11)], C['steel'], 1.1, 0.3, seed=83)
        wline(d, [(66, gy + 14), (88, gy + 12), (104, gy + 15)], C['steel'], 1.1, 0.3, seed=84)
        _pine(d, 10, gy + 12, 13, seed=36)
        _pine(d, 24, gy + 15, 10, seed=37)
        _pine(d, 96, gy + 13, 12, seed=43)
    elif kind == 'storm':
        _cloud(d, 6, 6, 44, 13, fill=C['steel'], shade=(85, 85, 85), seed=30)
        _cloud(d, 48, 12, 38, 12, fill=C['cloud_grey'], shade=(85, 85, 85), seed=39)
        _bolt(d, 36, 26, 1.1, seed=41)
        _bolt(d, 80, 30, 0.75, seed=42)
        rnd = random.Random(91)
        for i in range(5):
            rx = 16 + i * 20 + rnd.uniform(-3, 3)
            wline(d, [(rx, 36), (rx - 2.5, 44)], C['blue'], 1.5, 0.15, seed=91 + i)
    else:  # fog
        for i, fy in enumerate((26, 36, 46)):
            band = [(2, fy), (30, fy - 2.5), (66, fy + 2), (100, fy - 2), (114, fy + 1)]
            wline(d, band, C['white'], 5.0, 0.6, seed=95 + i)
            wline(d, band, C['ink'], 1.1, 0.6, seed=95 + i)
        _pine(d, 12, gy + 12, 12, seed=38)
        _pine(d, 96, gy + 13, 11, seed=40)

    # halftone print texture in the sky
    dot = tuple(min(255, c + 85) for c in sky)
    benday(d, (6, 2, SCENE_W - 2, 20), dot, 10, 0.9)

    # white gutters on the slanted left + bottom edges, then the wobbly ink frame
    d.polygon([xy(p) for p in [(-1, -1), (6, -1), (-1, SCENE_H - 9)]], fill=C['paper'])
    d.polygon([xy(p) for p in [(-1, SCENE_H - 9), (SCENE_W + 1, SCENE_H + 0.5),
                               (SCENE_W + 1, SCENE_H + 1), (-1, SCENE_H + 1)]], fill=C['paper'])
    wpoly(d, SCENE_POLY, None, C['ink'], 2.2, amp=0.4, seed=55)
    return pebblize(art_scale(im, SCENE_W, SCENE_H))

# ---------------------------------------------------------------- small icons
def _drop(d, cx, cy, r):
    """Fat comic raindrop: round belly + pointed top."""
    d.polygon([xy(p) for p in [(cx, cy - r * 1.7), (cx - r * 0.95, cy - r * 0.1),
                               (cx + r * 0.95, cy - r * 0.1)]], fill=C['blue'])
    ellipse(d, (cx - r, cy - r * 0.6, cx + r, cy + r * 1.4), C['blue'])
    ellipse(d, (cx - r, cy - r * 0.6, cx + r, cy + r * 1.4), None, C['ink'], 1.1)
    wline(d, [(cx, cy - r * 1.7), (cx - r * 0.95, cy - r * 0.1)], C['ink'], 1.1, 0.1, seed=int(cx * 7))
    wline(d, [(cx, cy - r * 1.7), (cx + r * 0.95, cy - r * 0.1)], C['ink'], 1.1, 0.1, seed=int(cx * 9))

def _flake(d, cx, cy, r):
    """Six-arm snowflake with dotted tips."""
    for a in range(0, 180, 60):
        rad = math.radians(a)
        wline(d, [(cx - math.cos(rad) * r, cy - math.sin(rad) * r),
                  (cx + math.cos(rad) * r, cy + math.sin(rad) * r)], C['white'], 1.7, 0.05, seed=int(cx))
        wline(d, [(cx - math.cos(rad) * r, cy - math.sin(rad) * r),
                  (cx + math.cos(rad) * r, cy + math.sin(rad) * r)], C['ink'], 0.7, 0.05, seed=int(cx))
    ellipse(d, (cx - 1.1, cy - 1.1, cx + 1.1, cy + 1.1), C['white'], C['ink'], 0.8)

def icon(kind, size=22):
    """Full-bleed 22px forecast icon with fat comic strokes."""
    im = Image.new('RGBA', (size * S, size * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if kind == 'clear':
        _sun(d, size / 2, size / 2, size * 0.30, nrays=8, seed=13, ray_len=size * 0.17, lw=1.7)
    elif kind == 'cloud':
        _sun(d, size * 0.66, size * 0.28, size * 0.19, nrays=8, seed=14, ray_len=size * 0.12, lw=1.5)
        _cloud(d, size * 0.00, size * 0.42, size * 0.96, size * 0.38, lw=1.7, seed=51)
    elif kind == 'rain':
        _cloud(d, size * 0.02, size * 0.10, size * 0.94, size * 0.36, lw=1.7, seed=52)
        for dx, dy in ((0.24, 0.76), (0.52, 0.86), (0.78, 0.74)):
            _drop(d, size * dx, size * dy, size * 0.085)
    elif kind == 'snow':
        _cloud(d, size * 0.02, size * 0.10, size * 0.94, size * 0.36, lw=1.7, seed=54)
        _flake(d, size * 0.36, size * 0.78, size * 0.18)
        ellipse(d, (size * 0.68, size * 0.76, size * 0.68 + 3.4, size * 0.76 + 3.4),
                C['white'], C['ink'], 1.0)
    elif kind == 'storm':
        _cloud(d, size * 0.02, size * 0.06, size * 0.94, size * 0.36, lw=1.7, seed=56)
        _bolt(d, size * 0.52, size * 0.40, size * 0.022, seed=57)
    else:  # fog
        _cloud(d, size * 0.02, size * 0.04, size * 0.94, size * 0.34, lw=1.7, seed=58)
        for i, yy in enumerate((0.60, 0.76, 0.92)):
            x0 = size * (0.08 + 0.05 * (i % 2))
            x1 = size * (0.92 - 0.05 * ((i + 1) % 2))
            wline(d, [(x0, size * yy), (x1, size * yy)], C['white'], 2.4, 0.15, seed=59 + i)
            wline(d, [(x0, size * yy), (x1, size * yy)], C['ink'], 1.0, 0.15, seed=59 + i)
    im = art_scale(im, size, size)
    r, g, b, a = im.split()
    rgb = Image.merge('RGB', (r, g, b)).point(_PEBBLE_LUT * 3)
    a = a.point(lambda v: 255 if v >= 128 else 0)
    rgb.putalpha(a)
    return rgb

# ---------------------------------------------------------------- PAGE GEOMETRY
# The page is cut on two tilted "horizon" lines (~4.3 deg) so the action band is
# a wedge, like a real comic action page. Shared with main.c (screen px):
#   chips           staggered, y 2..20
#   calendar panel  (0,20)-(88,22) top, bottom edge on line y = 82 + 0.075x
#   scene panel     blit rect (84,20,116,77); slanted bottom edge baked in PNG
#   action band     top edge (0,86)->(200,101), bottom edge (0,172)->(200,157)
#   TODAY panel     top edge (0,176)->(126,167)
#   TOMORROW panel  top edge (130,166)->(200,161)
# The starburst is drawn LAST and overlaps the gutters / panel borders.
CAL_POLY = [(0, 20), (88, 22), (80, 88), (0, 82)]
BAND_POLY = [(0, 86), (200, 101), (200, 157), (0, 172)]
TODAY_POLY = [(0, 176), (126, 167), (126, 228), (0, 228)]
TMRW_POLY = [(130, 166), (200, 161), (200, 228), (130, 228)]

def chip(im, d, x0, x1, tilt, seed, dy=0):
    layer = Image.new('RGBA', (sc(x1 - x0 + 8), sc(24)), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    wpoly(ld, [(4, 3), (x1 - x0 + 4, 3), (x1 - x0 + 4, 18), (4, 18)],
          C['white'] + (255,), C['ink'] + (255,), 1.7, amp=0.5, seed=seed)
    layer = layer.rotate(tilt, resample=Image.Resampling.BICUBIC, expand=False)
    im.paste(layer, (sc(x0 - 4), sc(-1.5 + dy)), layer)

def shoe_glyph(d, x, y):
    """Tiny comic sneaker, heel-left / toe-right, anchored top-left (x,y)."""
    body = [(x, y + 7), (x + 0.6, y + 0.5), (x + 4.4, y + 0.5), (x + 6.4, y + 4),
            (x + 10.5, y + 5), (x + 13.4, y + 6), (x + 14, y + 7)]
    wpoly(d, body, C['red'], C['ink'], 1.2, amp=0.2, seed=101)
    # white toe cap + sole
    d.polygon([xy(p) for p in [(x + 10.4, y + 5), (x + 13.4, y + 6), (x + 14, y + 7),
                               (x + 10.4, y + 7)]], fill=C['white'])
    rect(d, (x - 0.4, y + 7, x + 14.4, y + 9), C['white'], C['ink'], 1.2)
    wline(d, [(x + 5, y + 2.5), (x + 7.5, y + 4.5)], C['ink'], 1.0, 0.1, seed=103)

def heart_glyph(d, x, y):
    ellipse(d, (x, y, x + 5, y + 5), C['red'])
    ellipse(d, (x + 4, y, x + 9, y + 5), C['red'])
    d.polygon([xy(p) for p in [(x + 0.3, y + 3.4), (x + 8.7, y + 3.4), (x + 4.5, y + 8.5)]], fill=C['red'])
    pts = [(x + 0.3, y + 3.4), (x + 4.5, y + 8.5), (x + 8.7, y + 3.4)]
    wline(d, pts, C['ink'], 1.0, 0.15, seed=104)
    d.arc(box((x, y, x + 5, y + 5)), 130, 340, fill=C['ink'], width=sc(1.0))
    d.arc(box((x + 4, y, x + 9, y + 5)), 200, 50, fill=C['ink'], width=sc(1.0))

def battery_glyph(d, x, y):
    """Battery outline; main.c paints the level fill + %."""
    wpoly(d, [(x, y), (x + 18, y), (x + 18, y + 9), (x, y + 9)], C['white'], C['ink'], 1.5, amp=0.3, seed=105)
    rect(d, (x + 18.5, y + 2.5, x + 21, y + 6.5), C['ink'])

def calendar_art(im, d):
    rays_panel(im, CAL_POLY, (40, 54), C['purple'], (170, 85, 255), n=14, rot=12)
    wpoly(d, CAL_POLY, None, C['ink'], 2.2, amp=0.5, seed=110)
    # drop shadow
    d.polygon([xy(p) for p in [(13, 33), (75, 33), (75, 83), (13, 83)]], fill=C['red_dk'])
    # tear-off body
    body = [(9, 29), (71, 29), (71, 80), (9, 80)]
    wpoly(d, body, C['white'], C['ink'], 2.0, amp=0.5, seed=111)
    # red header band
    wpoly(d, [(9, 29), (71, 29), (71, 44), (9, 44)], C['red'], None, seed=112)
    wline(d, [(9, 44), (71, 44)], C['ink'], 1.8, 0.5, seed=113)
    wpoly(d, body, None, C['ink'], 2.0, amp=0.5, seed=111)
    # curled corner bottom-right
    d.polygon([xy(p) for p in [(60, 80), (71, 80), (71, 70)]], fill=C['steel'])
    wpoly(d, [(60, 80), (71, 70), (71, 80)], C['white'], None, seed=114)
    wline(d, [(60, 80), (71, 70)], C['ink'], 1.6, 0.4, seed=114)
    # spiral binder rings poking above the pad
    for rx in (22, 40, 58):
        ellipse(d, (rx - 3, 23, rx + 3, 31), None, C['white'], 1.6)
        ellipse(d, (rx - 3, 23, rx + 3, 31), None, C['ink'], 1.0)
        rect(d, (rx - 2, 27, rx + 2, 33), C['steel'], C['ink'], 1.1)

def band_art(im, d):
    rays_panel(im, BAND_POLY, (100, 129), C['red'], C['red_ray'], n=22, rot=4)
    wpoly(d, BAND_POLY, None, C['ink'], 2.4, amp=0.5, seed=120)
    sparkle(d, 15, 99, 5, C['yellow'], seed=123)
    sparkle(d, 186, 146, 4.5, C['yellow'], seed=124)
    sparkle(d, 178, 112, 3.2, C['white'], seed=125)
    sparkle(d, 10, 163, 3.4, C['white'], seed=126)

def burst_art(im, d):
    # Spikes that would leave the action band are shortened RADIALLY (staying
    # pointy, no flat clamp) so the burst fills its wedge edge-to-edge without
    # spilling onto the calendar / scene / forecast tiles. Bounds follow the
    # band's slanted edges (y = 86+0.075x top, 172-0.075x bottom) with a 2px
    # margin, plus 3px at the bottom for the burst's drop shadow.
    cx, cy = 100, 129
    bp = []
    for (px, py) in burst_pts(cx, cy, 101, 37, 68, 22, n=13, seed=9):
        for _ in range(2):  # bound depends on x, which moves when rescaling
            bound = (88 + 0.075 * px) if py < cy else (167 - 0.075 * px)
            if (py < cy and py < bound) or (py > cy and py > bound):
                t = (bound - cy) / (py - cy)
                px, py = cx + (px - cx) * t, float(bound)
        bp.append((px, py))
    d.polygon([xy((x + 2.5, y + 3)) for (x, y) in bp], fill=C['red_dk'])
    wpoly(d, bp, C['white'], C['ink'], 2.3, amp=0.6, seed=121)

def bottom_panels(im, d):
    # TODAY: cream + pink halftone, edges follow the tilted horizon
    d.polygon([xy(p) for p in TODAY_POLY], fill=C['cream'])
    benday(d, (4, 178, 123, 226), C['dot_pink'], 9, 1.0)
    wpoly(d, TODAY_POLY, None, C['ink'], 2.2, amp=0.5, seed=130)
    for cx, sd in ((32, 131), (62, 132), (92, 133)):
        wline(d, [(cx + 1, 174 - cx * 0.07), (cx - 1, 225)], C['ink'], 1.1, 0.5, seed=sd)
    # TOMORROW: orange rays
    rays_panel(im, TMRW_POLY, (165, 199), C['orange'], C['orange_ray'], n=12, rot=18)
    wpoly(d, TMRW_POLY, None, C['ink'], 2.2, amp=0.5, seed=134)

def captions_art(im):
    # pasted last; captions straddle their panel's top border and the gutter,
    # but stay clear of the action band above
    caption(im, 27, 173.5, 42, 12, 'TODAY', F_CAP12, C['red'], tilt=-4.0, seed=135)
    caption(im, 163, 167, 62, 12, 'TOMORROW', F_CAP10, C['blue'], tilt=3.0, seed=136)

def base_layout():
    im = Image.new('RGB', (W * S, H * S), C['paper'])
    d = ImageDraw.Draw(im)
    calendar_art(im, d)
    band_art(im, d)
    bottom_panels(im, d)
    burst_art(im, d)
    captions_art(im)
    # status chips (numbers drawn by main.c), staggered + tilted
    chip(im, d, 2, 64, -2.4, 141, dy=2)
    chip(im, d, 70, 116, 2.0, 142, dy=0)
    chip(im, d, 122, 198, -1.6, 143, dy=3)
    shoe_glyph(d, 6, 5.5)
    heart_glyph(d, 75, 3.5)
    battery_glyph(d, 126, 8)
    return pebblize(art_scale(im, W, H))

def export_assets():
    base_layout().save(RES / 'base_layout.png')
    for kind in ['clear', 'cloud', 'rain', 'snow', 'storm', 'fog']:
        scene(kind).save(RES / f'scene_{kind}.png')
        icon(kind, 22).save(RES / f'icon_{kind}_small.png')

# ---------------------------------------------------------------- mock preview
def cap_text(d, cx, cap_top, t, f, fill, stroke=0, shadow=0, shadow_fill=None, align='c'):
    """Place text with its VISIBLE glyph top at cap_top (Pebble-style top alignment)."""
    bb = d.textbbox((0, 0), t, font=f, stroke_width=sc(stroke))
    if align == 'c':
        x = sc(cx) - (bb[2] - bb[0]) / 2 - bb[0]
    elif align == 'l':
        x = sc(cx) - bb[0]
    else:
        x = sc(cx) - (bb[2] - bb[0]) - bb[0]
    y = sc(cap_top) - bb[1]
    if shadow:
        d.text((x + sc(shadow), y + sc(shadow)), t, font=f, fill=shadow_fill or C['ink'],
               stroke_width=sc(stroke), stroke_fill=shadow_fill or C['ink'])
    d.text((x, y), t, font=f, fill=fill, stroke_width=sc(stroke), stroke_fill=C['ink'])

def render_preview():
    export_assets()
    base = Image.open(RES / 'base_layout.png').convert('RGB').resize((W * S, H * S), Image.Resampling.NEAREST)
    scn = Image.open(RES / 'scene_clear.png').resize((SCENE_W * S, SCENE_H * S), Image.Resampling.NEAREST)
    base.paste(scn, (sc(84), sc(20)))
    d = ImageDraw.Draw(base)

    # --- chips: steps / hr / battery (mirrors prv_draw_status_chips)
    # cap positions = C GRect y + 5.5 (Pebble puts Lilita 14 caps ~5.5px below box top)
    cap_text(d, 23, 7.5, '8427', F_SMALL, C['ink'], align='l')
    cap_text(d, 88, 6.5, '72', F_SMALL, C['ink'], align='l')
    rect(d, (128, 10, 128 + 14, 15), C['batt_green'])
    cap_text(d, 150, 8.5, '100%', F_SMALL, C['ink'], align='l')

    # --- calendar (mirrors prv_draw_calendar)
    cap_text(d, 40, 31, 'JUN', F_SMALL, C['white'])
    cap_text(d, 40, 46, '28', F_BIG, C['ink'])
    cap_text(d, 40, 67, 'SUN', F_SMALL, C['ink'])

    # --- scene: temp + condition caption (mirrors prv_draw_weather_scene)
    cap_text(d, 192, 24, '25°', F_BIG, C['white'], stroke=0.55, align='r')
    cond = 'CLEAR'
    bb = d.textbbox((0, 0), cond, font=F_SMALL)
    cw = (bb[2] - bb[0]) / S + 8
    rect(d, (89, 74, 89 + cw, 87), C['ink'])
    cap_text(d, 89 + cw / 2, 76, cond, F_SMALL, C['white'])

    # --- time (mirrors prv_draw_center_panel)
    cap_text(d, 100, 113, '07:31', F_TIME, C['gold'], stroke=0.6, shadow=0.8, shadow_fill=C['gold_sh'])

    # --- today panel
    cols = [16, 47, 77, 108]
    hrs = [('08', '25', 'clear'), ('09', '26', 'cloud'), ('10', '27', 'cloud'), ('11', '28', 'clear')]
    for cx, (hh, tmp, k) in zip(cols, hrs):
        cap_text(d, cx, 181.5, hh, F_SMALL, C['ink'])
        ic = Image.open(RES / f'icon_{k}_small.png').convert('RGBA').resize((22 * S, 22 * S), Image.Resampling.NEAREST)
        base.paste(ic, (sc(cx - 11), sc(192)), ic)
        cap_text(d, cx, 214.5, tmp, F_MED, C['ink'])

    # --- tomorrow panel
    ic = Image.open(RES / 'icon_cloud_small.png').convert('RGBA').resize((22 * S, 22 * S), Image.Resampling.NEAREST)
    base.paste(ic, (sc(154), sc(180)), ic)
    cap_text(d, 165, 209.5, '24/34', F_MED, C['ink'])

    out = pebblize(base.resize((W, H), Image.Resampling.LANCZOS))
    out.save(PREV / 'mock_preview.png')
    out.resize((W * 3, H * 3), Image.Resampling.NEAREST).save(PREV / 'mock_preview_3x.png')

if __name__ == '__main__':
    render_preview()
    print('assets + preview regenerated')

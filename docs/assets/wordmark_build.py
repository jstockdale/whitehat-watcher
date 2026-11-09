from PIL import Image, ImageDraw, ImageFont
import numpy as np, os

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'
FONT = '/usr/share/fonts/truetype/noto/NotoSansDisplay-Black.ttf'
GREEN = (53, 208, 127)      # console green #35D07F
CREAM = (247, 242, 231)
CHARCOAL = (37, 32, 47)     # sampled from logo-alt1 bg
TEAL = (5, 71, 92)          # sampled deep teal
INK = (6, 58, 74)           # deep teal ink for dark-on-cream
SS = 3  # supersample

def tracked_size(draw, font, text, tracking):
    w = 0
    for ch in text:
        w += draw.textlength(ch, font=font) + tracking
    return w - tracking

def draw_tracked(draw, xy, font, text, tracking, fill):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x - tracking

def wordmark(bg, ink, fname, sub=None):
    W, H = 3200*SS, 800*SS
    img = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(img)
    fs = 400*SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs*0.22)
    tw = tracked_size(d, font, 'WATCHER', tracking)
    ascent, descent = font.getmetrics()
    cap = ascent  # approx text block height reference
    x0 = (W - tw)//2
    y0 = int(H*0.5 - fs*0.52)
    x1 = draw_tracked(d, (x0, y0), font, 'WATCHER', tracking, ink)
    # beam sweep: thin wedge under the word, apex at left, opening to the right
    base_y = y0 + int(fs*1.08)
    hh = int(fs*0.14)
    d.polygon([(x0, base_y), (x1, base_y), (x1, base_y+hh), (x0, base_y+int(hh*0.18))], fill=GREEN)
    img = img.resize((3200, 800), Image.LANCZOS)
    img.save(os.path.join(ASSETS, fname))
    print(fname, img.size)

# 1. dark on cream
wordmark(CREAM, INK, 'wordmark-dark.png')
# 2. cream on deep teal
wordmark(TEAL, CREAM, 'wordmark-light.png')

# ---------- lockup: extract the approved mark by keying out its charcoal bg ----------
src = Image.open(os.path.join(ASSETS, 'logo-alt1.png')).convert('RGB')
arr = np.asarray(src).astype(int)
bgc = np.array([37, 32, 47])
dist = np.abs(arr - bgc).max(axis=2)
mask = dist > 25
ys, xs = np.where(mask)
y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
print('mark bbox', x0, x1, y0, y1)
crop_rgb = arr[y0:y1+1, x0:x1+1]
crop_dist = dist[y0:y1+1, x0:x1+1]
alpha = np.clip((crop_dist - 10) * 3, 0, 255).astype(np.uint8)
mark = np.dstack([crop_rgb.astype(np.uint8), alpha])
mark_img = Image.fromarray(mark, 'RGBA')

W, H = 4200*SS, 1000*SS
canvas = Image.new('RGB', (W, H), CHARCOAL)
mh = int(H*0.92)
mw = int(mark_img.width * mh / mark_img.height)
mark_r = mark_img.resize((mw, mh), Image.LANCZOS)
mx = int(W*0.035)
my = (H - mh)//2
canvas.paste(mark_r, (mx, my), mark_r)

d = ImageDraw.Draw(canvas)
fs = 380*SS
font = ImageFont.truetype(FONT, fs)
tracking = int(fs*0.22)
tx = mx + mw + int(W*0.045)
ty = int(H*0.5 - fs*0.52)
draw_tracked(d, (tx, ty), font, 'WATCHER', tracking, (250, 250, 248))
out = canvas.resize((4200, 1000), Image.LANCZOS)
out.save(os.path.join(ASSETS, 'wordmark-lockup.png'))
print('wordmark-lockup.png', out.size)
from PIL import Image, ImageDraw, ImageFont
import os

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'
FONT = '/usr/share/fonts/truetype/noto/NotoSansDisplay-Black.ttf'
GREEN = (53, 208, 127)
CREAM = (247, 242, 231)
TEAL = (5, 71, 92)
INK = (6, 58, 74)
SS = 3

def draw_tracked(draw, xy, font, text, tracking, fill):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x - tracking

def build(bg, ink, fname):
    fs = 400*SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs*0.22)
    # build group on oversized transparent layer
    layer = Image.new('RGBA', (3400*SS, 900*SS), (0,0,0,0))
    d = ImageDraw.Draw(layer)
    tw = sum(d.textlength(c, font=font) for c in 'WATCHER') + tracking*6
    x0, y0 = 100*SS, 100*SS
    x1 = draw_tracked(d, (x0, y0), font, 'WATCHER', tracking, ink + (255,))
    gap = int(fs*0.10)
    base_y = y0 + int(fs*1.16) + gap
    hh = int(fs*0.13)
    d.polygon([(x0, base_y), (x1, base_y), (x1, base_y+hh), (x0, base_y+int(hh*0.15))], fill=GREEN + (255,))
    bbox = layer.getbbox()
    group = layer.crop(bbox)
    W, H = 3200*SS, 800*SS
    img = Image.new('RGB', (W, H), bg)
    img.paste(group, ((W-group.width)//2, (H-group.height)//2), group)
    img = img.resize((3200, 800), Image.LANCZOS)
    img.save(os.path.join(ASSETS, fname))
    print(fname, 'group', group.size)

build(CREAM, INK, 'wordmark-dark.png')
build(TEAL, CREAM, 'wordmark-light.png')

"""WATCHER wordmark variant: the flat Nobody lighthouse stands in for the T.

Extends the deterministic Pillow route of wordmark_build.py (same font,
weight, tracking, cream bg, deep-teal ink, console-green beam sweep) and
replaces the T glyph with the mascot extracted from mascot.png.

Extraction: mascot.png's background is a flat deep teal, and the character
is wrapped in a bold white outline, so a border flood-fill keys exactly the
connected background (the tower's own interior teal is enclosed by the
outline and survives). The foreground mask is eroded ~2px to eat the
antialiased teal blend at the outline's outer edge (no teal fringe on
cream), and only the main connected component is kept. The amber wave
flourish under the base hangs from a teal mound enclosed by the wave
stroke, so keying retains it; it is cropped away at the base platform's
bottom edge (the platform is the character's footing on the baseline).

Layout: the lighthouse occupies the T advance slot (same tracking rhythm),
base on the text baseline, total height 1.12x cap height so the captain's
hat rises just slightly above the cap line.
"""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import numpy as np, os

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'
FONT = '/usr/share/fonts/truetype/noto/NotoSansDisplay-Black.ttf'
GREEN = (53, 208, 127)      # console green #35D07F
CREAM = (247, 242, 231)
INK = (6, 58, 74)           # deep teal ink, as wordmark-dark.png
SS = 3                      # supersample, as the original builder


# ---------------------------------------------------------------- extraction
def extract_character():
    src = Image.open(os.path.join(ASSETS, 'mascot.png')).convert('RGB')
    W, H = src.size
    # flood-fill the border-connected background with a sentinel colour
    filled = src.copy()
    SENT = (255, 0, 255)
    for seed in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1),
                 (W // 2, 0), (W // 2, H - 1), (0, H // 2), (W - 1, H // 2)]:
        ImageDraw.floodfill(filled, seed, SENT, thresh=25)
    bg = (np.asarray(filled) == SENT).all(axis=2)
    fg = Image.fromarray((~bg).astype(np.uint8) * 255, 'L')
    # erode ~2px: removes the teal/cream antialias blend hugging the outline
    fg = fg.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MinFilter(3))
    # keep only the main component (seeded inside the tower); drops the wave
    comp = fg.copy()
    ImageDraw.floodfill(comp, (W // 2, int(H * 0.62)), 128, thresh=0)
    mask = (np.asarray(comp) == 128).astype(np.uint8) * 255
    alpha = Image.fromarray(mask, 'L').filter(ImageFilter.GaussianBlur(1.0))
    arr = np.dstack([np.asarray(src), np.asarray(alpha)])
    char = Image.fromarray(arr, 'RGBA')
    bbox = char.getbbox()
    char = char.crop(bbox)
    # The wave flourish under the base hangs from a teal mound that is
    # enclosed by the wave stroke, so keying keeps it. Crop it away at the
    # base platform's bottom edge: scan down from the platform's widest row
    # to the first sustained sharp width drop (platform ~500px -> neck ~300).
    a = np.asarray(char)[:, :, 3]
    widths = (a > 40).sum(axis=1)
    # platform = widest row in its band (80-96% of height); the wave rows
    # below can be wider still, so the search must not reach them
    lo, hi = int(len(widths) * 0.80), int(len(widths) * 0.96)
    plat = lo + int(np.argmax(widths[lo:hi]))
    pmax = widths[plat]
    # below the platform the silhouette necks sharply into the wave mound
    # (500px wide -> ~300px): cut at the first row under 75% of platform
    # width -- the platform keeps its full body, the mound and wave go.
    below = np.nonzero(widths[plat:] < 0.75 * pmax)[0]
    cut = plat + int(below[0]) if len(below) else len(widths)
    char = char.crop((0, 0, char.width, cut))
    char = char.crop(char.getbbox())
    print('character extracted:', char.size, 'bbox in source:', bbox,
          'wave cropped at row', cut, 'of', len(widths))
    return char


# ------------------------------------------------------------------ wordmark
def draw_tracked(draw, xy, font, text, tracking, fill):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x - tracking


def build_t(char):
    fs = 400 * SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs * 0.22)
    layer = Image.new('RGBA', (3400 * SS, 900 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0 = 100 * SS, 100 * SS

    # text geometry: baseline + cap height from the H glyph box
    hb = font.getbbox('H')                    # (l, top, r, bottom) vs draw origin
    cap_h = hb[3] - hb[1]
    baseline_y = y0 + hb[3]

    adv = {c: d.textlength(c, font=font) for c in 'WATCHER'}
    total = sum(adv.values()) + tracking * 6

    # lighthouse sized into the T slot
    target_h = int(cap_h * 1.12)
    target_w = int(char.width * target_h / char.height)
    char_r = char.resize((target_w, target_h), Image.LANCZOS)

    x = x0
    for ch in 'WATCHER':
        if ch == 'T':
            slot_cx = x + adv['T'] / 2
            px = int(slot_cx - target_w / 2)
            py = int(baseline_y - target_h)   # base sits on the baseline
            layer.paste(char_r, (px, py), char_r)
        else:
            d.text((x, y0), ch, font=font, fill=INK + (255,))
        x += adv[ch] + tracking
    x1 = x0 + total

    # beam sweep under the full word, identical to wordmark-dark.png
    gap = int(fs * 0.10)
    base_y = y0 + int(fs * 1.16) + gap
    hh = int(fs * 0.13)
    d.polygon([(x0, base_y), (x1, base_y), (x1, base_y + hh),
               (x0, base_y + int(hh * 0.15))], fill=GREEN + (255,))

    group = layer.crop(layer.getbbox())
    W, H = 3200 * SS, 800 * SS
    img = Image.new('RGB', (W, H), CREAM)
    img.paste(group, ((W - group.width) // 2, (H - group.height) // 2), group)
    img = img.resize((3200, 800), Image.LANCZOS)
    out = os.path.join(ASSETS, 'wordmark-t.png')
    img.save(out)
    print('wordmark-t.png', img.size, 'group', group.size,
          'cap_h', cap_h, 'lighthouse', char_r.size)


if __name__ == '__main__':
    char = extract_character()
    char.save('/tmp/nobody_extracted.png')          # inspection aid
    prev = Image.new('RGB', char.size, CREAM)
    prev.paste(char, (0, 0), char)
    prev.save('/tmp/nobody_on_cream.png')           # fringe check on cream
    build_t(char)

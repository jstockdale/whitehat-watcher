"""WATCHER wordmark, beam-T variant: HIGH-RES MASTER at 2x.

Identical layout to wordmark_build_t3.py (the current final,
wordmark-t-beam.png at 3200x800) but rendered as a 6400x1600 master:
the build supersample doubles (SS 3 -> 6, font 400*SS) and every
beam constant doubles with it, so the downscale factor to the final
PNG stays 3 and every proportion is unchanged. Letters are re-rasterised
from the font at twice the size -- letter-perfect, just sharper.
The extracted mascot is the same source; it is Lanczos-upscaled 2x
further than in the 3200 master (the mascot.png source is the limit
for character detail -- letters, beams and sweep gain the most).
"""
from PIL import Image, ImageDraw, ImageFont
import numpy as np, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wordmark_build_t import extract_character, FONT, GREEN, CREAM, INK
from wordmark_build_t2 import lamp_geometry, lighten
from wordmark_build_t3 import footing_edge, HEIGHT_FACTOR

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'

SS = 6                  # build supersample (t3 used 3); final = build/3
OUT_W, OUT_H = 6400, 1600

# beam tuning -- t3's constants doubled with the build scale
HALF_SPAN = 960         # t3: 480
ROOT_HALF = 96          # t3: 48
TIP_HALF = 190          # t3: 95
CORE_ROOT_HALF = 52     # t3: 26
CORE_TIP_HALF = 108     # t3: 54
ROOT_TUCK = 24          # t3: 12
BEAM_LIFT = 0


def build_hires(char):
    fs = 400 * SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs * 0.22)
    layer = Image.new('RGBA', (3400 * SS, 900 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0 = 100 * SS, 100 * SS

    hb = font.getbbox('H')
    cap_h = hb[3] - hb[1]
    baseline_y = y0 + hb[3]

    adv = {c: d.textlength(c, font=font) for c in 'WATCHER'}
    total = sum(adv.values()) + tracking * 6

    target_h = int(cap_h * HEIGHT_FACTOR)          # same 1.2096 x cap_h
    target_w = int(char.width * target_h / char.height)
    char_r = char.resize((target_w, target_h), Image.LANCZOS)
    print('cap_h', cap_h, 'char', char_r.size)

    slot_cx = x0 + adv['W'] + tracking + adv['A'] + tracking + adv['T'] / 2
    px = int(slot_cx - target_w / 2)
    fedge = footing_edge(char_r)
    py = baseline_y - fedge                        # footing ON the baseline
    print('footing edge', fedge, 'of', target_h, '; py', py,
          '; baseline_y', baseline_y)

    fx, fy, _ft, _fb, lamp_rgb = lamp_geometry(char)
    lamp_x = px + fx * target_w
    lamp_y = py + fy * target_h
    print('lamp rgb', lamp_rgb, '=',
          round((baseline_y - lamp_y) / cap_h, 3), 'cap-heights up')

    outer = lighten(lamp_rgb, 0.33) + (255,)
    core = lighten(lamp_rgb, 0.66) + (255,)

    # ---- beams first (roots covered by the tower) ----
    by = lamp_y - BEAM_LIFT
    for sgn in (-1, +1):
        tip_x = lamp_x + sgn * HALF_SPAN
        root_x = lamp_x - sgn * ROOT_TUCK
        d.polygon([(root_x, by - ROOT_HALF), (tip_x, by - TIP_HALF),
                   (tip_x, by + TIP_HALF), (root_x, by + ROOT_HALF)],
                  fill=outer)
        d.polygon([(root_x, by - CORE_ROOT_HALF),
                   (tip_x, by - CORE_TIP_HALF),
                   (tip_x, by + CORE_TIP_HALF), (root_x, by + CORE_ROOT_HALF)],
                  fill=core)

    # ---- letters + lighthouse ----
    x = x0
    letter_x = {}
    for ch in 'WATCHER':
        letter_x[ch] = x
        if ch == 'T':
            layer.paste(char_r, (px, py), char_r)
        else:
            d.text((x, y0), ch, font=font, fill=INK + (255,))
        x += adv[ch] + tracking
    x1 = x0 + total

    # ---- beam sweep, unchanged proportions ----
    gap = int(fs * 0.10)
    base_y = y0 + int(fs * 1.16) + gap
    hh = int(fs * 0.13)
    d.polygon([(x0, base_y), (x1, base_y), (x1, base_y + hh),
               (x0, base_y + int(hh * 0.15))], fill=GREEN + (255,))

    group = layer.crop(layer.getbbox())
    W, H = OUT_W * 3, OUT_H * 3                    # downscale factor 3, as t3
    img = Image.new('RGB', (W, H), CREAM)
    img.paste(group, ((W - group.width) // 2, (H - group.height) // 2), group)
    img = img.resize((OUT_W, OUT_H), Image.LANCZOS)
    out = os.path.join(ASSETS, 'wordmark-t-beam-hires.png')
    img.save(out)
    print('wordmark-t-beam-hires.png', img.size, 'group', group.size)
    return img


def verify_hires(img):
    """Hires downscaled to the 3200 master size should be near-identical
    to wordmark-t-beam.png (same layout; only rasterisation differs)."""
    ref = Image.open(os.path.join(ASSETS, 'wordmark-t-beam.png')).convert('RGB')
    small = img.resize(ref.size, Image.LANCZOS)
    diff = np.abs(np.asarray(small).astype(int) -
                  np.asarray(ref).astype(int))
    print('hires->3200 vs current final: mean|diff| %.3f  max %d  '
          'pixels>25: %.4f%%' % (diff.mean(), diff.max(),
                                 100.0 * (diff.max(axis=2) > 25).mean()))


if __name__ == '__main__':
    char = extract_character()
    img = build_hires(char)
    verify_hires(img)
    img.crop((2300, 120, 4100, 1120)).save('/tmp/tbeam_hires_zoom.png')

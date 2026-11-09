"""WATCHER wordmark variant 3: beam T-wordmark, final owner tweaks.

Extends wordmark_build_t2.py (same deterministic Pillow route, font,
tracking, cream bg, deep-teal ink, console-green beam sweep, same
extracted mascot, same amber lamp beams). Two adjustments over v2:

1. The lighthouse is ~8% BIGGER (height factor 1.12 -> 1.12*1.08 of cap
   height). The beams stay glued to the lamp room: the lamp position is
   re-detected from the character and the wedges are anchored to the
   actually-pasted lamp, so the crossbar rises a touch with the taller
   character instead of detaching from the lamp. BEAM_LIFT is now 0 and
   the tip half-height slightly reduced so the crossbar top stays close
   to the cap line.

2. The character's footing sits EXACTLY on the text baseline. The
   baseline is computed from the font metrics (bottom of the 'H' glyph
   box vs the draw origin), and the paste position is derived from the
   resized character's own footing edge -- the bottom edge of its lowest
   strongly-opaque row -- not from the image's bottom row (the extracted
   alpha is Gaussian-blurred, so the image edge sits a hair below the
   visible footing). verify_baseline() measures the lowest ink row of
   the W feet, the C, and the lighthouse footing in the composed layer
   and in the final downscaled PNG, and prints them.
"""
from PIL import Image, ImageDraw, ImageFont
import numpy as np, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wordmark_build_t import extract_character, FONT, GREEN, CREAM, INK, SS
from wordmark_build_t2 import lamp_geometry, lighten

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'

SCALE = 1.08           # owner's +5-10%, aim ~8%
HEIGHT_FACTOR = 1.12 * SCALE

# beam tuning (SS units) -- anchored to the lamp, as v2
HALF_SPAN = 480        # each beam's reach from slot centre (unchanged)
ROOT_HALF = 48         # wedge half-height at the lamp
TIP_HALF = 95          # wedge half-height at the flat-cut tip (was 105)
CORE_ROOT_HALF = 26    # lighter core wedge
CORE_TIP_HALF = 54     # (was 58)
BEAM_LIFT = 0          # wedge centre ON the lamp centroid (was 16): the
                       # taller character already raises the crossbar


def footing_edge(char_r):
    """Continuous y of the bottom edge of the character's footing: one
    past the lowest row that is strongly opaque (alpha >= 128) anywhere
    across the platform."""
    a = np.asarray(char_r)[:, :, 3]
    rows = np.nonzero((a >= 128).any(axis=1))[0]
    return int(rows.max()) + 1


def lowest_ink_row(arr_rgba, x_lo, x_hi, y_lo, y_hi):
    """Lowest row in the window whose alpha is strongly opaque."""
    win = arr_rgba[y_lo:y_hi, x_lo:x_hi, 3]
    rows = np.nonzero((win >= 128).any(axis=1))[0]
    return y_lo + int(rows.max()) if len(rows) else None


def build_t3(char):
    fs = 400 * SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs * 0.22)
    layer = Image.new('RGBA', (3400 * SS, 900 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0 = 100 * SS, 100 * SS

    hb = font.getbbox('H')
    cap_h = hb[3] - hb[1]
    baseline_y = y0 + hb[3]          # the font-metric baseline (SS px)

    adv = {c: d.textlength(c, font=font) for c in 'WATCHER'}
    total = sum(adv.values()) + tracking * 6

    # lighthouse, ~8% bigger than v2
    target_h = int(cap_h * HEIGHT_FACTOR)
    target_w = int(char.width * target_h / char.height)
    char_r = char.resize((target_w, target_h), Image.LANCZOS)
    print('cap_h', cap_h, 'char height: v2-equivalent', int(cap_h * 1.12),
          '-> v3', target_h, '(x%.3f)' % (target_h / (cap_h * 1.12)))

    slot_cx = x0 + adv['W'] + tracking + adv['A'] + tracking + adv['T'] / 2
    px = int(slot_cx - target_w / 2)
    fedge = footing_edge(char_r)
    py = baseline_y - fedge          # footing bottom edge ON the baseline
    print('footing edge at resized row', fedge, 'of', target_h,
          '; paste py', py, '; baseline_y', baseline_y)

    # lamp position from the ACTUAL paste origin, so beams stay attached
    fx, fy, _ftop, _fbot, lamp_rgb = lamp_geometry(char)
    lamp_x = px + fx * target_w
    lamp_y = py + fy * target_h
    print('lamp rgb', lamp_rgb, 'at layer', (round(lamp_x), round(lamp_y)),
          '=', round((baseline_y - lamp_y) / cap_h, 3), 'cap-heights up')

    outer = lighten(lamp_rgb, 0.33) + (255,)
    core = lighten(lamp_rgb, 0.66) + (255,)
    print('beam outer', outer[:3], 'core', core[:3])

    # ---- beams first (roots covered by the tower) ----
    by = lamp_y - BEAM_LIFT
    for sgn in (-1, +1):
        tip_x = lamp_x + sgn * HALF_SPAN
        root_x = lamp_x - sgn * 12
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

    # ---- verify baseline alignment at build (SS) scale ----
    arr = np.asarray(layer)
    band = (baseline_y - 80, baseline_y + 80)
    w_row = lowest_ink_row(arr, int(letter_x['W']), int(letter_x['W'] + adv['W']),
                           *band)
    c_row = lowest_ink_row(arr, int(letter_x['C']), int(letter_x['C'] + adv['C']),
                           *band)
    t_row = lowest_ink_row(arr, px, px + target_w, *band)
    print('SS-scale lowest ink rows: W', w_row, ' C', c_row,
          ' lighthouse', t_row, ' (baseline bottom edge =', baseline_y,
          '-> last ink row should be', baseline_y - 1, ')')

    # beam sweep under the full word, unchanged
    gap = int(fs * 0.10)
    base_y = y0 + int(fs * 1.16) + gap
    hh = int(fs * 0.13)
    d.polygon([(x0, base_y), (x1, base_y), (x1, base_y + hh),
               (x0, base_y + int(hh * 0.15))], fill=GREEN + (255,))

    # build-scale baseline zoom: W foot .. lighthouse footing, on cream
    zoom = layer.crop((int(letter_x['W']) - 40, baseline_y - 200,
                       px + target_w + 40, baseline_y + 120))
    zbg = Image.new('RGBA', zoom.size, CREAM + (255,))
    zbg.alpha_composite(zoom)
    zbg.convert('RGB').save('/tmp/tbeam3_baseline_zoom_ss.png')

    group = layer.crop(layer.getbbox())
    W, H = 3200 * SS, 800 * SS
    img = Image.new('RGB', (W, H), CREAM)
    img.paste(group, ((W - group.width) // 2, (H - group.height) // 2), group)
    img = img.resize((3200, 800), Image.LANCZOS)
    out = os.path.join(ASSETS, 'wordmark-t-beam.png')
    img.save(out)
    print('wordmark-t-beam.png', img.size, 'group', group.size)
    return img, baseline_y, group


def verify_final(img):
    """Lowest strong-ink row of W feet vs lighthouse footing in the
    saved 3200x800 PNG (ink = far from cream)."""
    a = np.asarray(img).astype(int)
    dist = np.abs(a - np.array(CREAM)).sum(axis=2)
    ink = dist > 120
    H, Wd = ink.shape
    # find the letters' baseline: the modal lowest-ink row over the
    # left half of the word (W and A feet), excluding the sweep band
    # (sweep is green and lower; restrict to rows above 0.62*H)
    rows = []
    for x in range(0, Wd, 4):
        col = np.nonzero(ink[:int(H * 0.62), x])[0]
        if len(col):
            rows.append(col.max())
    base = int(np.median(rows))
    def lowest(x_lo, x_hi):
        sub = ink[:int(H * 0.62), x_lo:x_hi]
        r = np.nonzero(sub.any(axis=1))[0]
        return int(r.max())
    # W occupies roughly the first letter slot; lighthouse the T slot
    print('final PNG: modal letter-foot row', base,
          '| W region lowest', lowest(250, 700),
          '| lighthouse region lowest', lowest(950, 1250))


if __name__ == '__main__':
    char = extract_character()
    img, _by, _g = build_t3(char)
    verify_final(img)
    img.crop((1150, 60, 2050, 560)).save('/tmp/tbeam3_zoom.png')
    img.resize((400, 100), Image.LANCZOS).save('/tmp/tbeam3_small.png')
    # baseline strip of the final render, magnified x6 (nearest)
    strip = img.crop((250, 300, 1400, 420)).resize((1150 * 2, 240),
                                                  Image.NEAREST)
    strip.save('/tmp/tbeam3_baseline_strip.png')

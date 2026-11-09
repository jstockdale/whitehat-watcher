"""WATCHER wordmark variant 2: the lighthouse in the T slot grows light beams.

Extends wordmark_build_t.py (same deterministic Pillow route, font, tracking,
cream bg, deep-teal ink, console-green beam sweep, same extracted mascot).
The owner's refinement: add a light beam on EACH side of the lamp room so
the character reads as the letter T -- the two beams form its crossbar.
Beams are warm yellow/amber light (sampled from the lamp glass itself),
NOT console green.

Beam geometry (build scale, SS=3 supersample):
- Origin: the lamp-glass centroid, re-detected at build time from the
  extracted character (bright amber pixels in its top 30%): frac
  (0.506, 0.233) of the character, i.e. ~0.86 cap-heights above the
  baseline -- exactly where a bold T's crossbar sits.
- Each beam is a flat wedge: narrow at the lamp, widening gently outward,
  flat-cut tip (a crossbar end). Total span ~960 units vs the real T
  glyph's 674 -- clearly wider than the tower, tips ending well inside
  the tracking gaps, never reaching the A or C.
- Two flat tones, no gradients: an outer pale-amber wedge (lamp amber
  lightened) around a lighter core wedge, like a stepped beam of light.
- Beams are drawn first; the character is pasted over their roots, so
  the light appears to pour out of the lamp room's sides.
"""
from PIL import Image, ImageDraw, ImageFont
import numpy as np, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wordmark_build_t import extract_character, FONT, GREEN, CREAM, INK, SS

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'

# beam tuning (SS units)
HALF_SPAN = 480        # each beam's reach from slot centre (total ~960)
ROOT_HALF = 48         # wedge half-height at the lamp
TIP_HALF = 105         # wedge half-height at the flat-cut tip
CORE_ROOT_HALF = 26    # lighter core wedge
CORE_TIP_HALF = 58
BEAM_LIFT = 16         # wedge centre sits this far above the lamp centroid,
                       # so the tips' top edge lands on the cap line like a
                       # real T crossbar; roots stay inside the lamp glass


def lamp_geometry(char):
    """Centroid + vertical extent of the lamp glass, as fractions of the
    character image. Bright amber in the top 30% is the lamp room."""
    arr = np.asarray(char).astype(int)
    rgb, a = arr[:, :, :3], arr[:, :, 3]
    H, W = a.shape
    mask = (a > 100) & (rgb[:, :, 0] > 200) & (rgb[:, :, 1] > 120) & \
           (rgb[:, :, 1] < 210) & (rgb[:, :, 2] < 120)
    mask[int(H * 0.30):, :] = False
    ys, xs = np.nonzero(mask)
    med = np.median(rgb[mask], axis=0).astype(int)
    lamp_rgb = tuple(int(v) for v in med)
    return (xs.mean() / W, ys.mean() / H,
            ys.min() / H, ys.max() / H, lamp_rgb)


def lighten(rgb, t):
    """Mix rgb toward warm white by t (0..1)."""
    target = (255, 246, 220)
    return tuple(int(round(c + (tc - c) * t)) for c, tc in zip(rgb, target))


def build_t2(char):
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

    # lighthouse sized into the T slot, exactly as wordmark_build_t.py
    target_h = int(cap_h * 1.12)
    target_w = int(char.width * target_h / char.height)
    char_r = char.resize((target_w, target_h), Image.LANCZOS)

    # the T slot's centre x
    slot_cx = x0 + adv['W'] + tracking + adv['A'] + tracking + adv['T'] / 2

    # lamp position in layer coordinates
    fx, fy, _ftop, _fbot, lamp_rgb = lamp_geometry(char)
    lamp_x = slot_cx + (fx - 0.5) * target_w
    lamp_y = baseline_y - target_h + fy * target_h
    print('lamp rgb', lamp_rgb, 'at layer', (round(lamp_x), round(lamp_y)),
          '=', round((baseline_y - lamp_y) / cap_h, 3), 'cap-heights up')

    outer = lighten(lamp_rgb, 0.33) + (255,)
    core = lighten(lamp_rgb, 0.66) + (255,)
    print('beam outer', outer[:3], 'core', core[:3])

    # ---- beams first (roots will be covered by the tower) ----
    by = lamp_y - BEAM_LIFT
    for sgn in (-1, +1):
        tip_x = lamp_x + sgn * HALF_SPAN
        root_x = lamp_x - sgn * 12          # tuck the root behind the glass
        d.polygon([(root_x, by - ROOT_HALF), (tip_x, by - TIP_HALF),
                   (tip_x, by + TIP_HALF), (root_x, by + ROOT_HALF)],
                  fill=outer)
        d.polygon([(root_x, by - CORE_ROOT_HALF),
                   (tip_x, by - CORE_TIP_HALF),
                   (tip_x, by + CORE_TIP_HALF),
                   (root_x, by + CORE_ROOT_HALF)], fill=core)

    # ---- letters + lighthouse, same loop as build_t ----
    x = x0
    for ch in 'WATCHER':
        if ch == 'T':
            px = int(slot_cx - target_w / 2)
            py = int(baseline_y - target_h)
            layer.paste(char_r, (px, py), char_r)
        else:
            d.text((x, y0), ch, font=font, fill=INK + (255,))
        x += adv[ch] + tracking
    x1 = x0 + total

    # beam sweep under the full word, unchanged
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
    out = os.path.join(ASSETS, 'wordmark-t-beam.png')
    img.save(out)
    print('wordmark-t-beam.png', img.size, 'group', group.size)
    return img


if __name__ == '__main__':
    char = extract_character()
    img = build_t2(char)
    # inspection aids: zoom on the T, and a small-size T-read check
    img.crop((1150, 60, 2050, 560)).save('/tmp/tbeam_zoom.png')
    img.resize((400, 100), Image.LANCZOS).save('/tmp/tbeam_small.png')
    img.resize((800, 200), Image.LANCZOS).save('/tmp/tbeam_mid.png')

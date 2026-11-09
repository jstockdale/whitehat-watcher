"""WATCHER wordmark, SMALL-SIZE variant: same layout, glyph lighthouse.

Same deterministic route and layout as wordmark_build_t3.py (same font,
tracking, cream bg, deep-teal ink, console-green sweep, lighthouse in
the T slot at the same 1.2096 x cap-height, footing exactly on the font
baseline) -- but the T is a REDRAWN simplified flat glyph instead of the
extracted mascot, designed to survive small display sizes:

- chunkier tower (aspect 0.46 vs the mascot's 0.422, bolder taper)
- exactly 2 bold amber stripes (the mascot has 3 thin ones + windows)
- ONE large amber lens disc with a bold ink pupil (no glass detail)
- captain's hat reduced to three bold shapes: crown, ink band, brim,
  with a deep-teal keyline so the white hat reads on the cream ground
- beams ~1.6x thicker than the master's, same two-tone amber step
- NO windows, no white outline, no thin detail anywhere

Output 1600x400 (half the master's pixel size, built at the same SS=3
supersample and downscaled straight to 1600x400).

Owner retune (2026-11-09): the glyph sat a little TOO TALL -- the top of
the beam crossbar rode ~9-10px above the letters' cap tops at 880px
(the detailed master's tips sit ~3px above). Fix: the glyph is shrunk
5% (SHRINK), anchored at the baseline so the footing does not move, and
the wedge TIP halves are trimmed (152 -> 112, core 86 -> 62) so the
crossbar's top edge runs essentially ON the cap line where it leaves
the tower and the tip corners land at the master's ~+3px. Root halves
scale with the glyph (78 -> 74, core 42 -> 40), so the beam thickness
at the lamp is unchanged in proportion. (A pure proportional shrink
cannot reach the line: the lamp sits at 0.94 cap-heights and the fat
1.6x tips dominate the overshoot -- the tip trim is the honest lever.)
"""
from PIL import Image, ImageDraw, ImageFont
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wordmark_build_t import FONT, GREEN, CREAM, INK, SS
from wordmark_build_t2 import lighten
from wordmark_build_t3 import HEIGHT_FACTOR

ASSETS = '/home/hatch/workspace/projects/watcher-input-assistant/docs/assets'

AMBER = (250, 178, 70)          # the lamp amber (#FAB246) sampled in t2
HAT = (252, 252, 250)           # hat white
OUT_W, OUT_H = 1600, 400

GLYPH_ASPECT = 0.46             # w/h -- chunkier than the mascot's 0.422

SHRINK = 0.95                   # owner: glyph 5% shorter, baseline-anchored

# beams (SS build units): master's t3 halves x~1.6, same reach.
# Root halves x SHRINK (thickness at the lamp proportional to the glyph);
# tip halves trimmed further so the crossbar top meets the cap line.
HALF_SPAN = 480
ROOT_HALF = 74                  # was 78 (x SHRINK); t3: 48
TIP_HALF = 112                  # was 152, retuned to the cap line; t3: 95
CORE_ROOT_HALF = 40             # was 42 (x SHRINK); t3: 26
CORE_TIP_HALF = 62              # was 86, retuned with TIP_HALF; t3: 54


def draw_glyph(d, px, py, w, h):
    """The simplified lighthouse, drawn in the box (px, py, w, h).
    Local y fractions run 0 (hat top) .. 1 (platform bottom = baseline).
    Returns the lamp centre in layer coordinates."""
    def X(f):
        return px + f * w

    def Y(f):
        return py + f * h

    ow = max(6, int(0.020 * h))             # keyline width for hat shapes
    ink = INK + (255,)
    amber = AMBER + (255,)
    hat = HAT + (255,)

    # ---- captain's hat: crown, band, brim (bold, keylined) ----
    d.polygon([(X(0.31), Y(0.088)), (X(0.69), Y(0.088)),
               (X(0.635), Y(0.006)), (X(0.365), Y(0.006))],
              fill=hat, outline=ink, width=ow)
    d.rectangle([X(0.30), Y(0.058), X(0.70), Y(0.090)], fill=ink)   # band
    d.rounded_rectangle([X(0.05), Y(0.088), X(0.95), Y(0.126)],
                        radius=0.019 * h, fill=hat, outline=ink, width=ow)

    # ---- lamp room: solid housing + one big lens disc ----
    d.rectangle([X(0.21), Y(0.128), X(0.79), Y(0.318)], fill=ink)
    lamp_cx, lamp_cy = X(0.5), Y(0.223)
    r = 0.20 * w
    d.ellipse([lamp_cx - r, lamp_cy - r, lamp_cx + r, lamp_cy + r],
              fill=amber)
    rp = 0.085 * w
    d.ellipse([lamp_cx - rp, lamp_cy - rp, lamp_cx + rp, lamp_cy + rp],
              fill=ink)

    # ---- deck rail under the lamp room ----
    d.rectangle([X(0.15), Y(0.318), X(0.85), Y(0.350)], fill=ink)

    # ---- tower: bold trapezoid, exactly 2 amber stripes ----
    top_hw, bot_hw = 0.31 * w, 0.435 * w     # half-widths at deck / base
    y_top, y_bot = 0.350, 0.945

    def hw(yf):
        t = (yf - y_top) / (y_bot - y_top)
        return top_hw + (bot_hw - top_hw) * t

    cx = X(0.5)
    d.polygon([(cx - top_hw, Y(y_top)), (cx + top_hw, Y(y_top)),
               (cx + bot_hw, Y(y_bot)), (cx - bot_hw, Y(y_bot))], fill=ink)
    for yc in (0.545, 0.735):                # two bold stripes, full-bleed
        th = 0.042                           # half-thickness (fraction of h)
        d.polygon([(cx - hw(yc - th), Y(yc - th)),
                   (cx + hw(yc - th), Y(yc - th)),
                   (cx + hw(yc + th), Y(yc + th)),
                   (cx - hw(yc + th), Y(yc + th))], fill=amber)

    # ---- base platform: footing, bottom edge = the baseline ----
    d.rectangle([X(0.015), Y(0.945), X(0.985), Y(1.0)], fill=ink)
    return lamp_cx, lamp_cy


def build_small():
    fs = 400 * SS
    font = ImageFont.truetype(FONT, fs)
    tracking = int(fs * 0.22)
    layer = Image.new('RGBA', (3400 * SS, 900 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0 = 100 * SS, 100 * SS

    hb = font.getbbox('H')
    cap_h = hb[3] - hb[1]
    baseline_y = y0 + hb[3]                  # font-metric baseline

    adv = {c: d.textlength(c, font=font) for c in 'WATCHER'}
    total = sum(adv.values()) + tracking * 6

    target_h = int(cap_h * HEIGHT_FACTOR * SHRINK)   # 5% shorter than final
    target_w = int(target_h * GLYPH_ASPECT)
    slot_cx = x0 + adv['W'] + tracking + adv['A'] + tracking + adv['T'] / 2
    px = int(slot_cx - target_w / 2)
    py = baseline_y - target_h               # glyph bottom IS its footing
    print('cap_h', cap_h, 'glyph', (target_w, target_h),
          'baseline_y', baseline_y)

    lamp_x = px + target_w / 2
    lamp_y = py + 0.223 * target_h

    outer = lighten(AMBER, 0.33) + (255,)
    core = lighten(AMBER, 0.66) + (255,)

    # ---- beams first (roots covered by the glyph) ----
    for sgn in (-1, +1):
        tip_x = lamp_x + sgn * HALF_SPAN
        root_x = lamp_x - sgn * 12
        d.polygon([(root_x, lamp_y - ROOT_HALF), (tip_x, lamp_y - TIP_HALF),
                   (tip_x, lamp_y + TIP_HALF), (root_x, lamp_y + ROOT_HALF)],
                  fill=outer)
        d.polygon([(root_x, lamp_y - CORE_ROOT_HALF),
                   (tip_x, lamp_y - CORE_TIP_HALF),
                   (tip_x, lamp_y + CORE_TIP_HALF),
                   (root_x, lamp_y + CORE_ROOT_HALF)], fill=core)

    # ---- letters + glyph ----
    x = x0
    for ch in 'WATCHER':
        if ch == 'T':
            draw_glyph(d, px, py, target_w, target_h)
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
    W, H = 3200 * SS, 800 * SS               # composed as the master...
    img = Image.new('RGB', (W, H), CREAM)
    img.paste(group, ((W - group.width) // 2, (H - group.height) // 2), group)
    img = img.resize((OUT_W, OUT_H), Image.LANCZOS)   # ...rendered at half
    out = os.path.join(ASSETS, 'wordmark-t-small.png')
    img.save(out)
    print('wordmark-t-small.png', img.size, 'group', group.size)
    return img


if __name__ == '__main__':
    img = build_small()
    img.crop((575, 30, 1025, 280)).save('/tmp/tsmall_zoom.png')
    img.resize((440, 110), Image.LANCZOS).save('/tmp/tsmall_440.png')

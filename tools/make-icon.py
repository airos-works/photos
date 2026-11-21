#!/usr/bin/env python3
"""Generate the Photos 64-unit HVIF icon and optional PNG previews.

Run: python3 tools/make-icon.py [--no-preview]
Two photographic prints, the front one a landscape under a sun. Outputs go to
this checkout's resources directory. Only previews require Pillow.
"""
from pathlib import Path
import argparse
import math
import sys

sys.dont_write_bytecode = True
import hvif

C = hvif.hex_color
K = 0.55228475


class Icon:
    def __init__(self):
        self.styles, self.paths, self.shapes = [], [], []
        self.matrix = (1, 0, 0, 1, 0, 0)

    def rotate(self, degrees, cx, cy):
        a = math.radians(degrees)
        c, s = math.cos(a), math.sin(a)
        self.matrix = (c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)

    def reset(self):
        self.matrix = (1, 0, 0, 1, 0, 0)

    def _t(self, p):
        a, b, c, d, e, f = self.matrix
        return (a * p[0] + c * p[1] + e, b * p[0] + d * p[1] + f)

    def solid(self, color, alpha=255):
        self.styles.append({'color': C(color, alpha)})
        return len(self.styles) - 1

    def linear(self, start, end, colors):
        self.styles.append(hvif.linear_gradient(self._t(start), self._t(end), [
            (i / (len(colors) - 1), C(c)) for i, c in enumerate(colors)]))
        return len(self.styles) - 1

    def radial(self, center, radius, stops, ratio=1):
        self.styles.append(hvif.radial_gradient(center, radius, [
            (offset, C(color, alpha)) for offset, color, alpha in stops], ratio))
        return len(self.styles) - 1

    def shape(self, style, points, detail=False):
        points = [tuple(self._t(q) for q in hvif.normalize_point(p)) for p in points]
        self.paths.append({'closed': True, 'points': points})
        shape = {'style': style, 'paths': [len(self.paths) - 1]}
        if detail:
            shape['lod'] = (32 / 63.75, 4.0)
        self.shapes.append(shape)

    def ellipse(self, style, cx, cy, rx, ry, **kw):
        self.shape(style, [
            ((cx + rx, cy), (cx + rx, cy - K * ry), (cx + rx, cy + K * ry)),
            ((cx, cy + ry), (cx + K * rx, cy + ry), (cx - K * rx, cy + ry)),
            ((cx - rx, cy), (cx - rx, cy + K * ry), (cx - rx, cy - K * ry)),
            ((cx, cy - ry), (cx - K * rx, cy - ry), (cx + K * rx, cy - ry)),
        ], **kw)

    def rounded(self, style, left, top, right, bottom, r, **kw):
        k = r * (1 - K)
        self.shape(style, [
            ((left + r, top), (left + k, top), (left + r, top)),
            ((right - r, top), (right - r, top), (right - k, top)),
            ((right, top + r), (right, top + k), (right, top + r)),
            ((right, bottom - r), (right, bottom - r), (right, bottom - k)),
            ((right - r, bottom), (right - k, bottom), (right - r, bottom)),
            ((left + r, bottom), (left + r, bottom), (left + k, bottom)),
            ((left, bottom - r), (left, bottom - k), (left, bottom - r)),
            ((left, top + r), (left, top + r), (left, top + k)),
        ], **kw)

    def data(self):
        return dict(styles=self.styles, paths=self.paths, shapes=self.shapes)


def app_icon():
    icon = Icon()
    shadow = icon.radial((33, 55), 28, [
        (0, '1c2735', 70), (.55, '1c2735', 38), (1, '1c2735', 0)], .26)
    icon.ellipse(shadow, 33, 55, 28, 7)
    ink = icon.solid('27313c')

    # The print behind: a sunset, turned to the left.
    icon.rotate(-11, 30, 32)
    icon.rounded(ink, 7, 9, 51, 49, 2.6)
    paper2 = icon.linear((8, 10), (50, 48), ['f4f1e8', 'cfd3d6'])
    icon.rounded(paper2, 8, 10, 50, 48, 1.8)
    dusk = icon.linear((11, 13), (11, 41), ['f7b267', 'f27059', '9b4a6b'])
    icon.rounded(dusk, 11, 13, 47, 41, .8)
    icon.reset()

    # The print in front: a landscape, turned a little to the right.
    icon.rotate(7, 34, 34)
    icon.rounded(ink, 11, 13, 59, 57, 2.8)
    paper = icon.linear((12, 14), (58, 56), ['ffffff', 'f3f2ec', 'd9dde0'])
    icon.rounded(paper, 12, 14, 58, 56, 2)
    sky = icon.linear((16, 18), (16, 46), ['3f9be3', '8fd0f5', 'e3f4fb'])
    icon.rounded(sky, 15.5, 17.5, 54.5, 47.5, .8)
    sun = icon.radial(icon._t((44.5, 26)), 6.2, [
        (0, 'fff7c2', 255), (.55, 'ffd95a', 255), (1, 'ffb62e', 255)])
    icon.ellipse(sun, 44.5, 26, 5, 5)
    far = icon.linear((30, 28), (40, 47), ['5c8fb5', '3c6f97'])
    icon.shape(far, [(27, 47.5), (40.5, 29.5), (47, 38), (54.5, 33), (54.5, 47.5)])
    near = icon.linear((18, 30), (30, 47), ['57b56b', '2f8554', '1f6a4a'])
    icon.shape(near, [(15.5, 47.5), (15.5, 41), (25.5, 28), (36, 42), (40, 47.5)])
    snow = icon.solid('ffffff', 235)
    icon.shape(snow, [(25.5, 28), (29.2, 33), (26.8, 32.2), (25.2, 34), (23.4, 32.4), (21.9, 32.7)],
               detail=True)
    gloss = icon.solid('ffffff', 150)
    icon.shape(gloss, [(12.8, 14.6), (57.2, 14.6), (57.2, 15.4), (12.8, 15.4)], detail=True)
    icon.reset()
    return icon.data()


def render(icon, size):
    scale = size / 64
    visible = dict(icon, shapes=[s for s in icon['shapes']
                   if s.get('lod', (0, 4))[0] <= scale <= s.get('lod', (0, 4))[1]])
    return hvif.preview(visible, size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-preview', action='store_true', help='write HVIF without Pillow')
    args = parser.parse_args()
    out = Path(__file__).resolve().parent.parent / 'resources'
    out.mkdir(exist_ok=True)
    encoded = hvif.encode(hvif.decode(hvif.encode(app_icon())))
    if len(encoded) >= 6000:
        raise ValueError('icon exceeds the 6 KB budget')
    decoded = hvif.decode(encoded)
    if hvif.encode(decoded) != encoded:
        raise ValueError('HVIF round-trip changed the icon')
    target = out / 'Photos.hvif'
    target.write_bytes(encoded)
    outputs = [target]
    if not args.no_preview:
        for size in (256, 32, 16):
            suffix = '' if size == 256 else f'-{size}'
            target = out / f'Photos{suffix}-preview.png'
            render(decoded, size).save(target)
            outputs.append(target)
    for target in outputs:
        print(f'{target.relative_to(out.parent)}: {target.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()

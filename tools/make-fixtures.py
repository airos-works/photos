#!/usr/bin/env python3
"""Generates test pictures (nothing personal, nothing photographed):

  tools/make-fixtures.py OUT_DIR [COUNT]

JPEGs with EXIF dates spread over several years, every EXIF orientation, GPS
on some, a PNG, a WebP, a TIFF and a larger JPEG; laid out like a camera card
(DCIM/100TEST/...) so the same tree serves the import tests. Needs Pillow.
"""
import math
import os
import sys
from datetime import datetime, timedelta

from PIL import Image, ImageDraw
from PIL.TiffImagePlugin import IFDRational as R

def picture(index, size, label):
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    hue = (index * 47) % 360
    for y in range(h):
        for x in range(w):
            r = int(127 + 127 * math.sin(math.radians(hue + x * 180 / w)))
            g = int(127 + 127 * math.sin(math.radians(hue + 120 + y * 180 / h)))
            b = int(127 + 127 * math.sin(math.radians(hue + 240 + (x + y) * 90 / (w + h))))
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(img)
    d.rectangle([w // 8, h // 8, w // 8 + w // 3, h // 8 + h // 6], fill=(255, 255, 255))
    d.text((w // 8 + 6, h // 8 + 6), label, fill=(0, 0, 0))
    d.polygon([(w - 10, 10), (w - 60, 10), (w - 10, 60)], fill=(255, 220, 0))  # top right marker
    return img

def exif_bytes(img, when, orientation=1, gps=None, make="Fixture", model="TestCam 1"):
    exif = img.getexif()
    exif[0x010F] = make
    exif[0x0110] = model
    exif[0x0112] = orientation
    exif[0x0132] = when.strftime("%Y:%m:%d %H:%M:%S")
    ifd = exif.get_ifd(0x8769)
    ifd[0x9003] = when.strftime("%Y:%m:%d %H:%M:%S")
    ifd[0x8827] = 200
    ifd[0x829A] = R(1, 250)
    ifd[0x829D] = R(28, 10)
    ifd[0x920A] = R(35, 1)
    if gps:
        lat, lon = gps
        g = exif.get_ifd(0x8825)
        def dms(v):
            v = abs(v); d = int(v); m = int((v - d) * 60); s = (v - d - m / 60) * 3600
            return (R(d, 1), R(m, 1), R(int(s * 100), 100))
        g[1] = "N" if lat >= 0 else "S"; g[2] = dms(lat)
        g[3] = "E" if lon >= 0 else "W"; g[4] = dms(lon)
    return exif.tobytes()

def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    card = os.path.join(out, "DCIM", "100TEST")
    os.makedirs(card, exist_ok=True)
    start = datetime(2019, 3, 14, 9, 26, 53)
    for i in range(count):
        when = start + timedelta(days=i * 97, hours=i * 5, minutes=i * 7)
        size = (640, 480) if i % 3 else (480, 640)
        orientation = (i % 8) + 1 if i < 8 else 1
        img = picture(i, size, "IMG %04d\n%s\no=%d" % (i + 1, when.strftime("%Y-%m-%d"), orientation))
        gps = (-33.8688 + i * 0.01, 151.2093 - i * 0.01) if i % 4 == 0 else None
        img.save(os.path.join(card, "IMG_%04d.JPG" % (i + 1)), quality=88,
                 exif=exif_bytes(img, when, orientation, gps))
    big = picture(100, (4000, 3000), "BIG 4000x3000")
    big.save(os.path.join(card, "IMG_9000.JPG"), quality=92,
             exif=exif_bytes(big, datetime(2025, 12, 24, 18, 0, 0)))
    picture(101, (800, 600), "PNG no exif").save(os.path.join(out, "screenshot one.png"))
    picture(102, (800, 600), "WEBP").save(os.path.join(out, "picture.webp"), quality=85)
    picture(103, (600, 800), "TIFF").save(os.path.join(out, "scan.tif"))
    picture(104, (640, 480), "ünïcödé name").save(os.path.join(out, "Füße im Sand.jpg"), quality=85)
    with open(os.path.join(out, "notes.txt"), "w") as f:
        f.write("not a picture\n")
    print("fixtures in", out)

if __name__ == "__main__":
    main()

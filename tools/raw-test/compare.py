#!/usr/bin/env python3
"""compare.py OUT.png LEFT RIGHT [HEIGHT]: two pictures side by side (the
camera's embedded JPEG next to rawtest's development, PPM or JPEG), for
looking at. Needs Pillow."""
import sys
from PIL import Image, ImageOps

out, left, right = sys.argv[1:4]
height = int(sys.argv[4]) if len(sys.argv) > 4 else 480


def load(path):
    image = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    width = max(1, round(image.width * height / image.height))
    return image.resize((width, height), Image.LANCZOS)


a, b = load(left), load(right)
sheet = Image.new("RGB", (a.width + b.width + 8, height), (40, 40, 40))
sheet.paste(a, (0, 0))
sheet.paste(b, (a.width + 8, 0))
sheet.save(out)
print(out, sheet.size)

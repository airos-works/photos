#!/usr/bin/env python3
"""Makes the pictures the camera layer is tested with (docs/DEVICES.md).

    make-fixtures.py OUT

writes OUT/phone (what a camera or phone holds: a DCIM tree for QEMU's
usb-mtp device), OUT/card.img (the same tree on a FAT32 volume for
usb-storage) and OUT/sha1.txt. The pictures are generated: gradients with
their names on them, EXIF dates and thumbnails, some awkward names and sizes.
Needs Pillow; card.img needs mkfs.vfat and mtools.
"""
import hashlib
import io
import os
import struct
import subprocess
import sys

from PIL import Image, ImageDraw


def ifd(entries, base):
    """A TIFF IFD at offset `base` (little endian): (bytes, end offset).
    entries: (tag, type, count, value bytes or int)."""
    head = struct.pack('<H', len(entries))
    data_at = base + 2 + 12 * len(entries) + 4
    body, extra = b'', b''
    for tag, kind, count, value in entries:
        if isinstance(value, int):
            field = struct.pack('<HHII' if kind == 4 else '<HHIHH', tag, kind,
                                count, *([value] if kind == 4 else [value, 0]))
        else:
            if len(value) <= 4:
                field = struct.pack('<HHI', tag, kind, count) + value.ljust(4, b'\0')
            else:
                field = struct.pack('<HHII', tag, kind, count, data_at + len(extra))
                extra += value + (b'\0' if len(value) % 2 else b'')
        body += field
    return head, body, extra, data_at + len(extra)


def exif_block(when, thumbnail):
    """APP1 payload: IFD0 (make, model, date, Exif IFD), IFD1 (thumbnail)."""
    date = when.encode() + b'\0'
    make, model = b'air/OS\0', b'Fixture Camera 1\0'
    # sizes first: IFD0 has 4 entries, the Exif IFD 1, IFD1 3
    ifd0_at = 8
    ifd0_entries = [(0x010F, 2, len(make), make), (0x0110, 2, len(model), model),
                    (0x0132, 2, len(date), date), (0x8769, 4, 1, 0)]
    _, _, _, exif_at = ifd(ifd0_entries, ifd0_at)
    exif_entries = [(0x9003, 2, len(date), date)]
    _, _, _, ifd1_at = ifd(exif_entries, exif_at)
    ifd1_entries = [(0x0103, 3, 1, 6), (0x0201, 4, 1, 0), (0x0202, 4, 1, len(thumbnail))]
    _, _, _, thumb_at = ifd(ifd1_entries, ifd1_at)

    ifd0_entries[3] = (0x8769, 4, 1, exif_at)
    ifd1_entries[1] = (0x0201, 4, 1, thumb_at)
    out = b'II*\0' + struct.pack('<I', ifd0_at)
    head, body, extra, _ = ifd(ifd0_entries, ifd0_at)
    out += head + body + struct.pack('<I', ifd1_at) + extra
    head, body, extra, _ = ifd(exif_entries, exif_at)
    out += head + body + struct.pack('<I', 0) + extra
    head, body, extra, _ = ifd(ifd1_entries, ifd1_at)
    out += head + body + struct.pack('<I', 0) + extra
    assert len(out) == thumb_at, (len(out), thumb_at)
    return b'Exif\0\0' + out + thumbnail


def picture(name, size, seed, noise=False):
    width, height = size
    if noise:
        image = Image.frombytes('RGB', size, os.urandom(width * height * 3))
    else:
        image = Image.new('RGB', size)
        pixels = image.load()
        for y in range(height):
            for x in range(width):
                pixels[x, y] = ((x * 255 // width + seed * 40) % 256,
                                (y * 255 // height) % 256, (seed * 70 + x + y) % 256)
    draw = ImageDraw.Draw(image)
    draw.rectangle((20, 20, 20 + 12 * len(name), 60), fill=(0, 0, 0))
    draw.text((30, 30), name, fill=(255, 255, 255))
    return image


def jpeg(image, when, quality=85):
    thumb = image.copy()
    thumb.thumbnail((160, 120))
    small = io.BytesIO()
    thumb.save(small, 'JPEG', quality=70)
    out = io.BytesIO()
    image.save(out, 'JPEG', quality=quality, exif=exif_block(when, small.getvalue()))
    return out.getvalue()


def main():
    out = sys.argv[1]
    phone = os.path.join(out, 'phone')
    files = {}
    for i in range(1, 13):
        name = 'IMG_%04d.JPG' % i
        when = '2026:11:%02d 10:%02d:00' % (i, i)
        files['DCIM/100TEST/' + name] = (jpeg(picture(name, (640, 480), i), when), when)
    files['DCIM/100TEST/Holiday photo 13.jpg'] = (
        jpeg(picture('Holiday photo 13', (640, 480), 13), '2026:10:01 08:00:00'),
        '2026:10:01 08:00:00')
    files['DCIM/100TEST/Straße 日本.jpg'] = (
        jpeg(picture('Strasse', (640, 480), 14), '2026:10:02 09:30:00'), '2026:10:02 09:30:00')
    # more than 16 MiB: noise does not compress
    files['DCIM/101TEST/BIG_0001.JPG'] = (
        jpeg(picture('BIG_0001.JPG', (5200, 3900), 15, noise=True), '2026:09:01 12:00:00', 95),
        '2026:09:01 12:00:00')
    # sizes that make the container (12 bytes of header more) end on a packet
    for size in (500, 1012, 1024 * 1024 - 12, 2 * 1024 * 1024 + 500):
        body = hashlib.sha256(b'%d' % size).digest() * (size // 32 + 1)
        files['DCIM/101TEST/EDGE%d.JPG' % size] = (b'\xff\xd8' + body[:size - 4] + b'\xff\xd9',
                                                   '2026:08:01 00:00:00')
    files['DCIM/100TEST/MVI_0020.MOV'] = (os.urandom(3 * 1024 * 1024 + 17), '2026:11:15 15:00:00')
    files['DCIM/100TEST/DSC_0030.NEF'] = (b'II*\0' + os.urandom(1024 * 1024), '2026:11:16 16:00:00')
    files['DCIM/100TEST/notes.txt'] = (b'not a picture\n', '2026:11:16 16:00:00')
    files['MISC/readme.txt'] = (b'nothing to import here\n', '2026:11:16 16:00:00')

    import time
    lines = []
    for path, (data, when) in sorted(files.items()):
        full = os.path.join(phone, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, 'wb') as f:
            f.write(data)
        stamp = time.mktime(time.strptime(when, '%Y:%m:%d %H:%M:%S'))
        os.utime(full, (stamp, stamp))
        lines.append('%s  %s  %d' % (hashlib.sha1(data).hexdigest(), path, len(data)))
    with open(os.path.join(out, 'sha1.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')

    card = os.path.join(out, 'card.img')
    if os.path.exists(card):
        os.remove(card)
    try:
        subprocess.run(['truncate', '-s', '96M', card], check=True)
        subprocess.run(['mkfs.vfat', '-F', '32', '-n', 'TESTCARD', card], check=True,
                       stdout=subprocess.DEVNULL)
        # mtools writes long names in the locale's character set: UTF-8
        utf8 = dict(os.environ, LC_ALL='C.UTF-8')
        for top in sorted(os.listdir(phone)):
            subprocess.run(['mcopy', '-i', card, '-s', '-m', os.path.join(phone, top), '::'],
                           check=True, env=utf8)
    except (OSError, subprocess.CalledProcessError) as error:
        print('card.img not made:', error)
    print(len(files), 'files in', phone)


if __name__ == '__main__':
    main()

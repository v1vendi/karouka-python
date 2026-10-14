#!/usr/bin/env python3
"""Compares two PNGs pixel by pixel (8-bit, non-interlaced, as karouka captures are).

    tools/pngdiff.py expected.png actual.png
    tools/pngdiff.py --tolerance DELTA[:PERCENT] expected.png actual.png
    tools/pngdiff.py --pixels "X,Y=#RRGGBB[~DELTA] ..." actual.png

Exit 0 when the decoded pixels are identical, 1 when they differ (size, or the count of differing
pixels and the first one), 2 when a file can't be read. tools/dev.sh check uses it to hold
multi-view captures to the single-view ones.

With --tolerance, a pixel differs only when a channel differs by more than DELTA, and the images
match while at most PERCENT (default 0) percent of the pixels differ; the largest channel delta is
printed either way. tools/dev.sh check holds the software backend's captures to the Metal ones so:
the two rasterize anti-aliased edges and gradients slightly differently.

With --pixels, the PNG's pixel at each (X,Y) must have the RGB colour given, each channel within
DELTA (default 0); alpha is ignored. Exit 0 when every sample matches, 1 when one doesn't (every
sample is printed), 2 when the spec or the file can't be read. tools/dev.sh check holds a test
page's captures (alone, software, host io, multi-view) to its karouka-check-pixels meta so:
what only paint shows, such as a selection highlight, which no title can read
back.

It needs only the standard library. When Pillow is installed, a differing pair is decoded and
compared in C, with the same verdict and output: ~0.06 s a pair against ~1 s in pure Python for two
800x600 captures at load ~30. tools/wpt.py imports compare() for its reftests.
"""

import struct
import sys
import zlib

CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}  # grey, RGB, grey+alpha, RGBA
PIL_MODES = {1: "L", 3: "RGB", 4: "RGBA"}  # the modes ImageChops.difference takes, by channel count
_pil = None


def pil():
    """Pillow's (Image, ImageChops), or None without Pillow. Imported only once a pair differs: the
    import costs ~60 ms a process, and most of check's comparisons are of identical files."""
    global _pil
    if _pil is None:
        try:
            from PIL import Image, ImageChops
            _pil = (Image, ImageChops)
        except ImportError:
            _pil = ()
    return _pil or None


def read_png(path):
    """The header fields and the inflated, still filtered image data."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, header = 8, [], None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError("no IHDR")
    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in CHANNELS or interlace:
        raise ValueError(f"unsupported PNG (depth {depth}, color type {color}, interlace {interlace})")
    return width, height, CHANNELS[color], zlib.decompress(b"".join(idat))


def add_bytes(a, b, low, high):
    """a + b byte by byte, modulo 256, as one big-int addition: the low 7 bits of each byte add
    without carrying into the next byte, and the top bits are XORed back in (SWAR)."""
    x, y = int.from_bytes(a, "big"), int.from_bytes(b, "big")
    return bytearray((((x & low) + (y & low)) ^ ((x ^ y) & high)).to_bytes(len(a), "big"))


def unfilter(width, height, bpp, raw):
    stride = width * bpp
    pixels = bytearray(height * stride)
    prev = bytearray(stride)
    # Most rows of a capture are filtered Up (2), which adds the row above byte by byte: done in one
    # big-int addition (add_bytes) instead of a Python loop per byte.
    low, high = int.from_bytes(b"\x7f" * stride, "big"), int.from_bytes(b"\x80" * stride, "big")
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1 : start + 1 + stride])
        if kind == 2:
            line = add_bytes(line, prev, low, high)
        elif kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif kind != 0:
            raise ValueError(f"bad filter {kind} on row {y}")
        pixels[y * stride : (y + 1) * stride] = line
        prev = line
    return width, height, bpp, bytes(pixels)


def decode(path, png):
    """PNG's pixels (read_png's result for the file at PATH), as unfilter returns them: by Pillow
    when it can, otherwise by unfilter."""
    width, height, bpp, _ = png
    if bpp in PIL_MODES and pil():
        try:
            with pil()[0].open(path) as image:
                if image.size == (width, height) and image.mode == PIL_MODES[bpp]:
                    return width, height, bpp, image.tobytes()
        except (OSError, ValueError):
            pass  # unfilter decides, and reports what it can't read
    return unfilter(*png)


def differing_rows(width, height, channels, p1, p2):
    """The byte ranges of the rows that differ: an equal row is skipped by one C-speed compare."""
    stride = width * channels
    for start in range(0, height * stride, stride):
        if p1[start : start + stride] != p2[start : start + stride]:
            yield start, start + stride


def parse_pixels(spec):
    """[(x, y, (r, g, b), delta)] from "X,Y=#RRGGBB[~DELTA] ...". Raises ValueError."""
    samples = []
    for word in spec.split():
        where, _, color = word.partition("=")
        color, _, delta = color.partition("~")
        x, y = (int(v) for v in where.split(","))
        if len(color) != 7 or color[0] != "#":
            raise ValueError(f"bad colour in {word!r}")
        rgb = tuple(int(color[i : i + 2], 16) for i in (1, 3, 5))
        samples.append((x, y, rgb, int(delta or 0)))
    if not samples:
        raise ValueError("no samples")
    return samples


def check_pixels(spec, path):
    try:
        samples = parse_pixels(spec)
    except ValueError as error:
        print(f"pngdiff: --pixels {spec!r}: {error}", file=sys.stderr)
        return 2
    try:
        width, height, channels, pixels = decode(path, read_png(path))
    except (OSError, ValueError, zlib.error) as error:
        print(f"pngdiff: {error}", file=sys.stderr)
        return 2
    if channels < 3:
        print(f"pngdiff: {path} has no colour channels", file=sys.stderr)
        return 2
    failed = 0
    for x, y, rgb, delta in samples:
        if not (0 <= x < width and 0 <= y < height):
            print(f"pngdiff: pixel ({x},{y}) is outside the {width}x{height} image")
            failed += 1
            continue
        at = (y * width + x) * channels
        got = tuple(pixels[at : at + 3])
        ok = all(abs(a - b) <= delta for a, b in zip(got, rgb))
        failed += not ok
        print(f"pngdiff: pixel ({x},{y}) {'#%02x%02x%02x' % got} expected {'#%02x%02x%02x' % rgb}"
              f"~{delta}: {'ok' if ok else 'differs'}")
    return 1 if failed else 0


def main(argv):
    if len(argv) == 4 and argv[1] == "--pixels":
        return check_pixels(argv[2], argv[3])
    tolerance = None
    if len(argv) == 5 and argv[1] == "--tolerance":
        try:
            delta, _, percent = argv[2].partition(":")
            tolerance = (int(delta), float(percent or 0))
        except ValueError:
            print(__doc__.strip(), file=sys.stderr)
            return 2
        argv = argv[:1] + argv[3:]
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        expected, actual = read_png(argv[1]), read_png(argv[2])
    except (OSError, ValueError, zlib.error) as error:
        print(f"pngdiff: {error}", file=sys.stderr)
        return 2
    # The same size and the same filtered rows are the same pixels. Captures of the same pixels by
    # one encoder filter the same way, so most of check's comparisons end here, without the
    # per-byte unfiltering below (~0.25 s for two 800x600 captures).
    if expected == actual:
        return 0
    try:
        expected, actual = decode(argv[1], expected), decode(argv[2], actual)
    except ValueError as error:
        print(f"pngdiff: {error}", file=sys.stderr)
        return 2
    (w1, h1, c1, p1), (w2, h2, c2, p2) = expected, actual
    if (w1, h1, c1) != (w2, h2, c2):
        print(f"pngdiff: size differs: {w1}x{h1}x{c1} vs {w2}x{h2}x{c2}")
        return 1
    if p1 == p2:
        return 0
    if tolerance is not None:
        return compare_with_tolerance(w1, h1, c1, p1, p2, *tolerance)
    differing, first = 0, None
    for row_start, row_end in differing_rows(w1, h1, c1, p1, p2):
        for i in range(row_start, row_end, c1):
            if p1[i : i + c1] != p2[i : i + c1]:
                differing += 1
                if first is None:
                    first = i // c1
    x, y = first % w1, first // w1
    at = first * c1
    print(f"pngdiff: {differing} of {w1 * h1} pixels differ; first at ({x},{y}): "
          f"{tuple(p1[at : at + c1])} vs {tuple(p2[at : at + c1])}")
    return 1


def delta_counts(width, height, channels, p1, p2, delta):
    """(pixels with a channel delta over DELTA, the largest channel delta, the index of the first
    such pixel or None) of two equal-sized pixel buffers: by Pillow when it can, else in Python."""
    if channels in PIL_MODES and pil():
        return tolerance_counts_pil(width, height, channels, p1, p2, delta)
    differing, largest, first = 0, 0, None
    for row_start, row_end in differing_rows(width, height, channels, p1, p2):
        for i in range(row_start, row_end, channels):
            if p1[i : i + channels] == p2[i : i + channels]:
                continue
            pixel_delta = max(abs(a - b) for a, b in zip(p1[i : i + channels], p2[i : i + channels]))
            largest = max(largest, pixel_delta)
            if pixel_delta > delta:
                differing += 1
                if first is None:
                    first = i // channels
    return differing, largest, first


def compare(a_path, b_path):
    """(size_mismatch, max_channel_delta, differing_pixels) of two PNG files, for tools/wpt.py's
    fuzzy-reftest judgement. A size or channel-count mismatch is (True, 255, pixels of the first).
    Raises OSError, ValueError or zlib.error for a file that can't be read."""
    expected, actual = read_png(a_path), read_png(b_path)
    if expected == actual:
        return False, 0, 0
    (w1, h1, c1, p1), (w2, h2, c2, p2) = decode(a_path, expected), decode(b_path, actual)
    if (w1, h1, c1) != (w2, h2, c2):
        return True, 255, w1 * h1
    if p1 == p2:
        return False, 0, 0
    differing, largest, _ = delta_counts(w1, h1, c1, p1, p2, 0)
    return False, largest, differing


def compare_with_tolerance(width, height, channels, p1, p2, delta, percent):
    differing, largest, first = delta_counts(width, height, channels, p1, p2, delta)
    share = 100.0 * differing / (width * height)
    where = f"; first at ({first % width},{first // width})" if first is not None else ""
    verdict = "match" if share <= percent else "differ"
    print(f"pngdiff: {differing} of {width * height} pixels ({share:.3f}%) differ by more than {delta}, "
          f"largest channel delta {largest}{where}: {verdict} (tolerance {delta}:{percent})")
    return 0 if share <= percent else 1


def tolerance_counts_pil(width, height, channels, p1, p2, delta):
    """compare_with_tolerance's counts by Pillow: (pixels with a channel delta over DELTA, the
    largest channel delta, the index of the first such pixel or None)."""
    Image, ImageChops = pil()
    mode = PIL_MODES[channels]
    diff = ImageChops.difference(Image.frombytes(mode, (width, height), p1),
                                 Image.frombytes(mode, (width, height), p2))
    # A pixel's delta is its largest channel's: the per-pixel max over the bands.
    bands = diff.split()
    pixel_delta = bands[0]
    for band in bands[1:]:
        pixel_delta = ImageChops.lighter(pixel_delta, band)
    histogram = pixel_delta.histogram()
    largest = max(i for i, count in enumerate(histogram) if count or i == 0)
    # Identical pixels never count, whatever DELTA is (the pure-Python loop skips them).
    over = max(delta, 0)
    differing = sum(histogram[over + 1 :])
    first = None
    if differing:
        mask = pixel_delta.point(lambda v: 255 if v > over else 0)
        top = mask.getbbox()[1]
        first = top * width + mask.crop((0, top, width, top + 1)).getbbox()[0]
    return differing, largest, first


if __name__ == "__main__":
    sys.exit(main(sys.argv))

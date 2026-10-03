#!/usr/bin/env python3
"""check_screenshot.py IMAGE R,G,B -- every sample point of the screenshot
has the expected colour (corners, edge midpoints and centre, 4 px inside the
border). Exit 0 when they all match within a small tolerance."""
import sys
from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
want = tuple(int(v) for v in sys.argv[2].split(","))
w, h = img.size
points = [(x, y) for x in (4, w // 2, w - 5) for y in (4, h // 2, h - 5)]
bad = [(p, img.getpixel(p)) for p in points
       if any(abs(a - b) > 8 for a, b in zip(img.getpixel(p), want))]
for p, c in bad:
    print(f"  {sys.argv[1]}: pixel {p} is {c}, expected {want}")
sys.exit(1 if bad else 0)

# SPDX-License-Identifier: Apache-2.0
"""Renders packaging/linux/openmine-suite.svg's design to openmine-suite.ico (Pillow), so the
Windows icon and the Linux icon are the same artwork.  python3 packaging/windows/make_icon.py"""
from pathlib import Path
from PIL import Image, ImageDraw

S = 4  # supersampling
W = 256 * S
img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
p = lambda pts: [(x * S, y * S) for x, y in pts]
d.rounded_rectangle([8 * S, 8 * S, 248 * S, 248 * S], radius=44 * S, fill="#1d232b")
d.polygon(p([(128, 46), (206, 90), (128, 134), (50, 90)]), fill="#3fa9f5")
d.polygon(p([(50, 90), (128, 134), (128, 214), (50, 170)]), fill="#2b7cb8")
d.polygon(p([(206, 90), (128, 134), (128, 214), (206, 170)]), fill="#1f5c8a")
d.line(p([(72, 112), (150, 156), (184, 136), (106, 92), (78, 108)]), fill="#ffb300", width=10 * S, joint="curve")
for x, y in [(150, 156), (184, 136), (106, 92), (78, 108)]:
    d.ellipse([(x - 5) * S, (y - 5) * S, (x + 5) * S, (y + 5) * S], fill="#ffb300")
d.ellipse([(72 - 9) * S, (112 - 9) * S, (72 + 9) * S, (112 + 9) * S], fill="#ffb300")
icon = img.resize((256, 256), Image.LANCZOS)
out = Path(__file__).with_name("openmine-suite.ico")
icon.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
icon.save(Path(__file__).with_name("openmine-suite-256.png"))
print(out)

#!/usr/bin/env python3
"""Render the Jungle Leopard Display marks into every raster the build uses.

    python3 tools/make_icons.py

Needs Inkscape 1.x on PATH and Pillow. Run it from anywhere; paths are
relative to the repository root. The outputs are committed, so a build or
tools/release.ps1 never needs Python. Re-run it after editing anything in
assets/brand/ and commit the results together.

Which SVG feeds which size (see docs/brand.md):
  16 px          assets/brand/jungle-leopard-small.svg  (pixel-aligned 16 grid)
  24 px          assets/brand/jungle-leopard-24.svg     (pixel-aligned 24 grid)
  32 px and up   assets/brand/jungle-leopard.svg

Outputs:
  JLDisplayManager/app.ico                    16/24/32/48/64/128/256
  Jungle Leopard Display/app.ico              same file, for the CLI's .rc
  assets/brand/jungle-leopard-{64,128,256}.png
  installer/wizard-{100,200}.bmp              Inno Setup side image  (164x314 / 328x628)
  installer/wizard-small-{100,200}.bmp        Inno Setup corner mark (55x55 / 110x110)
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BRAND = ROOT / "assets" / "brand"
INSTALLER = ROOT / "installer"

MAIN = BRAND / "jungle-leopard.svg"
SMALL = {16: BRAND / "jungle-leopard-small.svg", 24: BRAND / "jungle-leopard-24.svg"}
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

# KruxSYS tokens used by the wizard art (docs/brand.md).
BG = "#07090c"
LINE = "#1a212c"
LINE_2 = "#2a3442"
TEXT = "#e8ecf2"
MUTED = "#8e9bb0"
ACCENT = "#22d3ee"


def inkscape() -> str:
    exe = shutil.which("inkscape")
    if not exe:
        sys.exit("make_icons: inkscape not found on PATH")
    return exe


def render(svg: Path, w: int, h: int, out: Path) -> Image.Image:
    subprocess.run(
        [inkscape(), str(svg), "-w", str(w), "-h", str(h), "-o", str(out)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return Image.open(out).convert("RGBA")


def icon_frame(size: int, tmp: Path) -> Image.Image:
    svg = SMALL.get(size, MAIN)
    return render(svg, size, size, tmp / f"icon-{size}.png")


def wizard_svg(w: int, h: int) -> str:
    """The tall side image: brand page background, the mark, the name, the
    KruxSYS wordmark. Drawn at the 100% size in a viewBox so the 200% render
    is the same picture, twice as sharp."""
    mark = MAIN.read_text(encoding="utf-8")
    inner = mark[mark.index(">") + 1: mark.rindex("</svg>")]
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 164 314" width="{w}" height="{h}">
  <rect width="164" height="314" fill="{BG}"/>
  <path d="M12 0V314M152 0V314" stroke="{LINE}" stroke-width="1"/>
  <g transform="translate(42 58) scale(1.25)">{inner}</g>
  <text x="82" y="168" text-anchor="middle" font-family="Segoe UI, DejaVu Sans, sans-serif"
        font-size="15" font-weight="700" fill="{TEXT}">Jungle Leopard</text>
  <text x="82" y="187" text-anchor="middle" font-family="Segoe UI, DejaVu Sans, sans-serif"
        font-size="15" font-weight="700" fill="{TEXT}">Display</text>
  <path d="M52 204H112" stroke="{LINE_2}" stroke-width="1"/>
  <text x="82" y="226" text-anchor="middle" font-family="Segoe UI, DejaVu Sans, sans-serif"
        font-size="11" fill="{MUTED}">by <tspan font-weight="700" fill="{TEXT}">Krux</tspan><tspan
        font-family="DejaVu Sans Mono" fill="{ACCENT}">SYS</tspan></text>
  <path d="M20 32V20H32M144 282V294H132" fill="none" stroke="{ACCENT}" stroke-width="2"/>
</svg>
"""


def to_bmp(img: Image.Image, out: Path, bg: str = BG) -> None:
    """Inno Setup wants 24-bit BMPs: flatten onto the page colour."""
    flat = Image.new("RGB", img.size, bg)
    flat.paste(img, mask=img.split()[3])
    flat.save(out, format="BMP")


def main() -> None:
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)

        # --- app.ico: one hand-picked frame per size, not one image resampled.
        frames = [icon_frame(s, tmp) for s in ICO_SIZES]
        ico = ROOT / "JLDisplayManager" / "app.ico"
        frames[-1].save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
                        append_images=frames[:-1])
        shutil.copyfile(ico, ROOT / "Jungle Leopard Display" / "app.ico")

        # Prove every frame landed as itself (Pillow silently resamples the
        # largest image if append_images is ignored).
        with Image.open(ico) as check:
            got = sorted(check.info["sizes"])
            want = sorted((s, s) for s in ICO_SIZES)
            if got != want:
                sys.exit(f"make_icons: app.ico has {got}, want {want}")
            check.size = (16, 16)
            small = check.convert("RGBA")
            if small.tobytes() != frames[0].tobytes():
                sys.exit("make_icons: 16 px frame is not the pixel-aligned design")

        # --- PNGs for the README, docs and anything else that wants a raster.
        for s in (64, 128, 256):
            render(MAIN, s, s, BRAND / f"jungle-leopard-{s}.png")

        # --- Inno Setup wizard art at 100% and 200%.
        INSTALLER.mkdir(exist_ok=True)
        for scale, suffix in ((1, "100"), (2, "200")):
            svg = tmp / f"wizard-{suffix}.svg"
            svg.write_text(wizard_svg(164 * scale, 314 * scale), encoding="utf-8")
            to_bmp(render(svg, 164 * scale, 314 * scale, tmp / f"wizard-{suffix}.png"),
                   INSTALLER / f"wizard-{suffix}.bmp")
            small_px = 55 * scale
            to_bmp(render(MAIN, small_px, small_px, tmp / f"wizard-small-{suffix}.png"),
                   INSTALLER / f"wizard-small-{suffix}.bmp", bg="#ffffff")

    print("make_icons: wrote app.ico (x2), 3 PNGs, 4 wizard BMPs")


if __name__ == "__main__":
    main()

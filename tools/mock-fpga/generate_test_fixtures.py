#!/usr/bin/env python3
"""Generate deterministic mock FPGA image fixtures.

The C++ mock server reads the PGM files in this directory. PNG copies are only
for quick visual inspection by researchers and developers.
"""

from __future__ import annotations

import json
import shutil
import struct
import zlib
from pathlib import Path


WIDTH = 800
HEIGHT = 600
PIXEL_COUNT = WIDTH * HEIGHT
SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "test-fixtures"


def clamp(value: int) -> int:
    return max(0, min(255, int(value)))


def hash_pixel(x: int, y: int, seed: int) -> int:
    value = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    value ^= value >> 13
    value = (value * 1274126177) & 0xFFFFFFFF
    value ^= value >> 16
    return value & 0xFFFFFFFF


def broad_line_contribution(x: int, center: int, strength: int, half_width: int) -> int:
    dx = x - center
    width2 = half_width * half_width
    return (strength * width2) // (dx * dx + width2)


def build_spectrum(seed: int, variant: int) -> bytearray:
    centers = [
        [88, 176, 292, 421, 558, 698],
        [62, 205, 318, 462, 610, 735],
        [116, 244, 372, 496, 646, 724],
        [74, 188, 352, 510, 632, 758],
        [98, 226, 338, 455, 584, 710],
        [132, 264, 404, 536, 668, 748],
    ]
    strengths = [
        [42, 64, 50, 72, 58, 44],
        [36, 54, 68, 48, 62, 40],
        [52, 46, 74, 54, 42, 56],
        [44, 60, 42, 70, 48, 38],
        [40, 58, 52, 66, 46, 54],
        [34, 62, 44, 58, 72, 36],
    ]
    widths = [
        [18, 24, 22, 28, 24, 20],
        [22, 26, 30, 24, 28, 22],
        [20, 24, 32, 26, 24, 20],
        [24, 28, 22, 30, 26, 22],
        [18, 26, 24, 28, 24, 20],
        [24, 30, 22, 26, 32, 24],
    ]

    profile = variant % len(centers)
    image = bytearray(PIXEL_COUNT)
    for y in range(HEIGHT):
        vertical_gradient = (y * (8 + variant % 4)) // HEIGHT
        soft_vignetting = (abs(y - HEIGHT // 2) * 5) // (HEIGHT // 2)
        row_base = y * WIDTH
        for x in range(WIDTH):
            value = 28 + vertical_gradient - soft_vignetting
            value += (x * (6 + variant % 5)) // WIDTH
            for line in range(6):
                value += broad_line_contribution(
                    x,
                    centers[profile][line],
                    strengths[profile][line],
                    widths[profile][line],
                )
            value += (hash_pixel(x, y, seed) % 7) - 3
            image[row_base + x] = clamp(value)
    return image


def scale_image(image: bytearray, numerator: int, denominator: int, offset: int = 0) -> bytearray:
    denominator = denominator or 1
    return bytearray(clamp((pixel * numerator) // denominator + offset) for pixel in image)


def add_hot_pixels(image: bytearray, seed: int, count: int, value: int = 214) -> None:
    state = seed or 1
    for _ in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        x = 1 + state % (WIDTH - 2)
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        y = 1 + state % (HEIGHT - 2)
        image[y * WIDTH + x] = clamp(value)


def add_saturation_block(image: bytearray, left: int, top: int, width: int, height: int) -> None:
    for y in range(top, min(top + height, HEIGHT)):
        row_base = y * WIDTH
        for x in range(left, min(left + width, WIDTH)):
            image[row_base + x] = 255


def add_stable_calibration_defects(image: bytearray, seed: int, flat_like: bool) -> None:
    add_hot_pixels(image, seed + 0x5A17, 30, 216 if flat_like else 218)
    row = 120 + (seed % 180)
    delta = -24 if flat_like else 18
    row_base = row * WIDTH
    for x in range(WIDTH):
        image[row_base + x] = clamp(image[row_base + x] + delta)


def build_calibration_frame(seed: int, base: int, amplitude: int, flat_like: bool) -> bytearray:
    image = bytearray(PIXEL_COUNT)
    for y in range(HEIGHT):
        row_base = y * WIDTH
        for x in range(WIDTH):
            value = base
            if flat_like:
                dx = x - WIDTH // 2
                dy = y - HEIGHT // 2
                radial = (dx * dx) // 19000 + (dy * dy) // 14000
                value += amplitude - radial
                value += (x * 8) // WIDTH
            else:
                value += (y * 3) // HEIGHT
            value += (hash_pixel(x, y, seed) % 5) - 2
            image[row_base + x] = clamp(value)
    add_stable_calibration_defects(image, seed, flat_like)
    return image


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + chunk_type
        + data
        + struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
    )


def write_png(path: Path, image: bytearray) -> None:
    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)  # filter type 0
        start = y * WIDTH
        rows.extend(image[start:start + WIDTH])

    ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 0, 0, 0, 0)
    payload = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(payload)


def write_pgm(path: Path, image: bytearray) -> None:
    path.write_bytes(f"P5\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + bytes(image))


def save_image(relative_stem: str, image: bytearray, manifest: list[dict[str, str]]) -> None:
    pgm_path = OUTPUT_DIR / f"{relative_stem}.pgm"
    png_path = OUTPUT_DIR / f"{relative_stem}.png"
    pgm_path.parent.mkdir(parents=True, exist_ok=True)
    write_pgm(pgm_path, image)
    write_png(png_path, image)
    manifest.append({
        "pgm": str(pgm_path.relative_to(OUTPUT_DIR)).replace("\\", "/"),
        "png": str(png_path.relative_to(OUTPUT_DIR)).replace("\\", "/"),
    })


def reset_output_dir() -> None:
    resolved = OUTPUT_DIR.resolve()
    allowed_parent = SCRIPT_DIR.resolve()
    if resolved.parent != allowed_parent:
        raise RuntimeError(f"Refusing to delete unexpected fixture path: {resolved}")
    if OUTPUT_DIR.exists():
        shutil.rmtree(OUTPUT_DIR)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)


def main() -> None:
    reset_output_dir()
    manifest: dict[str, object] = {
        "width": WIDTH,
        "height": HEIGHT,
        "pixelFormat": "RAW16_LOW12 generated from 8-bit PGM when sent by spectra_bridge_test.exe",
        "readoutOrder": "GLUX1605_HDR_4LANE_INTERLEAVED_EFFECTIVE",
        "scenes": {},
    }

    scenes: dict[str, list[dict[str, str]]] = {}

    normal: list[dict[str, str]] = []
    normal_pass = build_spectrum(101, 0)
    normal_warning = build_spectrum(202, 1)
    add_hot_pixels(normal_warning, 909, 120)
    normal_fail = build_spectrum(303, 2)
    add_saturation_block(normal_fail, 360, 120, 80, 80)
    save_image("normal/normal_pass", normal_pass, normal)
    normal[-1]["expectedQuality"] = "PASS"
    save_image("normal/normal_warning_hot_pixels", normal_warning, normal)
    normal[-1]["expectedQuality"] = "WARNING"
    normal[-1]["reason"] = "120 deterministic hot pixels"
    save_image("normal/normal_fail_saturation", normal_fail, normal)
    normal[-1]["expectedQuality"] = "FAIL"
    normal[-1]["reason"] = "80x80 saturated block"
    scenes["normal"] = normal

    hdr: list[dict[str, str]] = []
    hdr_pass_hg = build_spectrum(401, 0)
    hdr_pass_lg = scale_image(hdr_pass_hg, 1, 4, 4)
    hdr_warning_hg = build_spectrum(402, 1)
    add_hot_pixels(hdr_warning_hg, 1909, 120)
    hdr_warning_lg = scale_image(hdr_warning_hg, 1, 4, 4)
    hdr_fail_hg = build_spectrum(403, 2)
    hdr_fail_lg = scale_image(hdr_fail_hg, 1, 4, 4)
    add_saturation_block(hdr_fail_hg, 360, 120, 80, 80)
    add_saturation_block(hdr_fail_lg, 360, 120, 80, 80)
    for name, hg, lg, expected in [
        ("hdr_pass", hdr_pass_hg, hdr_pass_lg, "PASS"),
        ("hdr_warning", hdr_warning_hg, hdr_warning_lg, "WARNING"),
        ("hdr_fail", hdr_fail_hg, hdr_fail_lg, "FAIL"),
    ]:
        item: list[dict[str, str]] = []
        save_image(f"hdr/{name}_hg", hg, item)
        save_image(f"hdr/{name}_lg", lg, item)
        hdr.append({
            "hgPgm": item[0]["pgm"],
            "hgPng": item[0]["png"],
            "lgPgm": item[1]["pgm"],
            "lgPng": item[1]["png"],
            "expectedQuality": expected,
        })
    scenes["hdr"] = hdr

    dark: list[dict[str, str]] = []
    flat: list[dict[str, str]] = []
    hdr_dark: list[dict[str, str]] = []
    hdr_flat: list[dict[str, str]] = []
    for index in range(1, 9):
        suffix = f"{index:02d}"
        save_image(
            f"dark/dark_{suffix}",
            build_calibration_frame(1700 + index * 17, 7, 0, False),
            dark,
        )
        dark[-1]["usage"] = "NORMAL_DARK calibration sample"

        save_image(
            f"flat/flat_{suffix}",
            build_calibration_frame(2700 + index * 17, 132, 42, True),
            flat,
        )
        flat[-1]["usage"] = "NORMAL_FLAT calibration sample"

        item = []
        save_image(
            f"hdr_dark/hdr_dark_{suffix}_hg",
            build_calibration_frame(3700 + index * 19, 8, 0, False),
            item,
        )
        save_image(
            f"hdr_dark/hdr_dark_{suffix}_lg",
            build_calibration_frame(4700 + index * 23, 3, 0, False),
            item,
        )
        hdr_dark.append({
            "hgPgm": item[0]["pgm"],
            "hgPng": item[0]["png"],
            "lgPgm": item[1]["pgm"],
            "lgPng": item[1]["png"],
            "usage": "HDR_DARK calibration sample; HG/LG are used separately",
        })

        item = []
        save_image(
            f"hdr_flat/hdr_flat_{suffix}_hg",
            build_calibration_frame(5700 + index * 19, 145, 48, True),
            item,
        )
        save_image(
            f"hdr_flat/hdr_flat_{suffix}_lg",
            build_calibration_frame(6700 + index * 23, 48, 18, True),
            item,
        )
        hdr_flat.append({
            "hgPgm": item[0]["pgm"],
            "hgPng": item[0]["png"],
            "lgPgm": item[1]["pgm"],
            "lgPng": item[1]["png"],
            "usage": "HDR_FLAT calibration sample; HG/LG are used separately",
        })

    scenes["dark"] = dark
    scenes["flat"] = flat
    scenes["hdr-dark"] = hdr_dark
    scenes["hdr-flat"] = hdr_flat
    manifest["scenes"] = scenes
    (OUTPUT_DIR / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Generated fixtures in {OUTPUT_DIR}")
    print(f"PGM files: {len(list(OUTPUT_DIR.rglob('*.pgm')))}")
    print(f"PNG files: {len(list(OUTPUT_DIR.rglob('*.png')))}")


if __name__ == "__main__":
    main()

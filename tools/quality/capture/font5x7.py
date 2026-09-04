"""A tiny built-in 5x7 bitmap font.

The synthetic generator draws text panels, and text is exactly the content that
breaks 4:2:0 chroma and low-QP intra coding, so it has to be in the test
material.  The font is built in rather than loaded from the system (or from
Pillow's default) for one reason: **determinism**.  The same seed must produce
byte-identical frames on every machine and in every CI container, otherwise
PSNR numbers are not comparable across runs.

Glyphs are 5 wide by 7 tall, one string per row, ``#`` set and ``.`` clear.
"""

from __future__ import annotations

import numpy as np

GLYPH_W, GLYPH_H = 5, 7

_GLYPHS: dict[str, str] = {
    "A": ".###.|#...#|#...#|#####|#...#|#...#|#...#",
    "B": "####.|#...#|#...#|####.|#...#|#...#|####.",
    "C": ".###.|#...#|#....|#....|#....|#...#|.###.",
    "D": "####.|#...#|#...#|#...#|#...#|#...#|####.",
    "E": "#####|#....|#....|####.|#....|#....|#####",
    "F": "#####|#....|#....|####.|#....|#....|#....",
    "G": ".###.|#...#|#....|#.###|#...#|#...#|.###.",
    "H": "#...#|#...#|#...#|#####|#...#|#...#|#...#",
    "I": ".###.|..#..|..#..|..#..|..#..|..#..|.###.",
    "J": "..###|...#.|...#.|...#.|...#.|#..#.|.##..",
    "K": "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#",
    "L": "#....|#....|#....|#....|#....|#....|#####",
    "M": "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#",
    "N": "#...#|##..#|#.#.#|#..##|#...#|#...#|#...#",
    "O": ".###.|#...#|#...#|#...#|#...#|#...#|.###.",
    "P": "####.|#...#|#...#|####.|#....|#....|#....",
    "Q": ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#",
    "R": "####.|#...#|#...#|####.|#.#..|#..#.|#...#",
    "S": ".###.|#...#|#....|.###.|....#|#...#|.###.",
    "T": "#####|..#..|..#..|..#..|..#..|..#..|..#..",
    "U": "#...#|#...#|#...#|#...#|#...#|#...#|.###.",
    "V": "#...#|#...#|#...#|#...#|#...#|.#.#.|..#..",
    "W": "#...#|#...#|#...#|#.#.#|#.#.#|##.##|#...#",
    "X": "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#",
    "Y": "#...#|#...#|.#.#.|..#..|..#..|..#..|..#..",
    "Z": "#####|....#|...#.|..#..|.#...|#....|#####",
    "0": ".###.|#...#|#..##|#.#.#|##..#|#...#|.###.",
    "1": "..#..|.##..|..#..|..#..|..#..|..#..|.###.",
    "2": ".###.|#...#|....#|...#.|..#..|.#...|#####",
    "3": "#####|...#.|..#..|...#.|....#|#...#|.###.",
    "4": "...#.|..##.|.#.#.|#..#.|#####|...#.|...#.",
    "5": "#####|#....|####.|....#|....#|#...#|.###.",
    "6": "..##.|.#...|#....|####.|#...#|#...#|.###.",
    "7": "#####|....#|...#.|..#..|.#...|.#...|.#...",
    "8": ".###.|#...#|#...#|.###.|#...#|#...#|.###.",
    "9": ".###.|#...#|#...#|.####|....#|...#.|.##..",
    " ": ".....|.....|.....|.....|.....|.....|.....",
    ".": ".....|.....|.....|.....|.....|.##..|.##..",
    ",": ".....|.....|.....|.....|.##..|.##..|.#...",
    "-": ".....|.....|.....|#####|.....|.....|.....",
    "+": ".....|..#..|..#..|#####|..#..|..#..|.....",
    ":": ".....|.##..|.##..|.....|.##..|.##..|.....",
    "/": "....#|...#.|...#.|..#..|.#...|.#...|#....",
    "%": "##..#|##.#.|..#..|.#...|#....|#.##.|#.##.",
    "(": "..#..|.#...|#....|#....|#....|.#...|..#..",
    ")": "..#..|...#.|....#|....#|....#|...#.|..#..",
    "*": ".....|#.#.#|.###.|#####|.###.|#.#.#|.....",
    "=": ".....|.....|#####|.....|#####|.....|.....",
    "!": "..#..|..#..|..#..|..#..|..#..|.....|..#..",
    "?": ".###.|#...#|....#|...#.|..#..|.....|..#..",
    "#": ".#.#.|#####|.#.#.|.#.#.|#####|.#.#.|.....",
}

_UNKNOWN = "#####|#...#|#...#|#...#|#...#|#...#|#####"


def glyph(ch: str) -> np.ndarray:
    """A (7, 5) bool mask for one character (case-insensitive)."""
    rows = _GLYPHS.get(ch.upper(), _UNKNOWN).split("|")
    return np.array([[c == "#" for c in r] for r in rows], dtype=bool)


def text_mask(text: str, scale: int = 1, spacing: int = 1) -> np.ndarray:
    """A bool mask for *text*, each glyph pixel expanded to *scale* squared."""
    if not text:
        return np.zeros((GLYPH_H * scale, 1), dtype=bool)
    parts = []
    for i, ch in enumerate(text):
        if i:
            parts.append(np.zeros((GLYPH_H, spacing), dtype=bool))
        parts.append(glyph(ch))
    m = np.concatenate(parts, axis=1)
    if scale > 1:
        m = np.repeat(np.repeat(m, scale, axis=0), scale, axis=1)
    return m


def text_size(text: str, scale: int = 1, spacing: int = 1) -> tuple[int, int]:
    """(height, width) in pixels that :func:`text_mask` would produce."""
    if not text:
        return (GLYPH_H * scale, scale)
    w = len(text) * GLYPH_W + (len(text) - 1) * spacing
    return (GLYPH_H * scale, w * scale)


def draw_text(
    img: np.ndarray,
    text: str,
    y: int,
    x: int,
    colour=(255, 255, 255),
    scale: int = 1,
    spacing: int = 1,
) -> None:
    """Stamp *text* into an (H, W, 3) uint8 image at top-left (y, x). Clipped."""
    m = text_mask(text, scale, spacing)
    h, w = m.shape
    H, W = img.shape[:2]
    y0, x0 = max(0, y), max(0, x)
    y1, x1 = min(H, y + h), min(W, x + w)
    if y1 <= y0 or x1 <= x0:
        return
    sub = m[y0 - y : y1 - y, x0 - x : x1 - x]
    img[y0:y1, x0:x1][sub] = np.asarray(colour, dtype=np.uint8)

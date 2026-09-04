<div align="center">

<img src="nx-warp-logo.png" width="520" alt="NX Warp">

</div>

<br>

# The NX Warp brand

NX Warp is part of the [NX family](https://github.com/nerdrx/nx-hub), so it does
not have a visual language of its own. It has the NX one, applied to a codec.
The canonical specification lives in NX Hub at `docs/DESIGN.md` and that document
wins every disagreement with this one. What follows is only the part that is
specific to this repository: which file to reach for, and the small number of
ways you can get it wrong.

<br>

## The files

| File | Use it for | Do not |
|---|---|---|
| `nx-warp-mark.svg` | The mark on its own at 48px and larger. Full bevel, sunken well, refracted edges. | Render it below 48px. |
| `nx-warp-mark-small.svg` | 16 to 32px. Bevel, well and cyan casing dropped, grid coarsened to 3x3. | Use it large. It is a flat tile and it looks like one. |
| `nx-warp-mark-mono.svg` | One-colour contexts that inherit a text colour (`currentColor`). | Recolour the grid separately. It is a knockout, not a fill. |
| `nx-warp-mark-mono-white.svg` | Dark grounds, one-ink print, OS tinting. | |
| `nx-warp-mark-mono-black.svg` | Paper and light UI chrome. | |
| `nx-warp-favicon.svg` | Browser tab. The small variant on a true-black rounded tile. | |
| `nx-warp-logo.svg` / `.png` | The horizontal lockup: mark, wordmark, micro-label. Dark grounds only. | Rebuild the lockup by hand. The spacing is part of it. |
| `nx-warp-logo-mono.svg` | One-colour lockup. Inherits `currentColor`, so it works on either ground. | |
| `nx-warp-social.svg` / `.png` | The 1280x640 GitHub social preview and any other card at that ratio. | Crop it. The clear space is load bearing. |
| `nx-warp-mark.png`, `nx-warp-mark-128.png` | Places that cannot take SVG (some Markdown renderers, some launchers). | Treat as source. The SVGs are the source. |

Every SVG here is hand-authored vector: paths, gradients, one Gaussian blur.
There is no traced raster and no embedded bitmap anywhere. The PNGs are exported
from the SVGs with `rsvg-convert` and can be regenerated at any size.

<br>

## What the mark is

A pointy-top beveled glass hexagon, the same solid every NX app is cut from,
with a monogram sculpted from the same glass. NX Hub's monogram is the letters
NX. NX Warp's is a **4x4 tile grid under a reprojection**: it enters at the left
square, the way the frame was rendered, and leaves at the right sheared,
shortened and bowed, the way the pose warp puts it back down. Two tiles are lit,
because this codec refreshes tiles and not frames.

The warp is a function, not a drawing. Both the mark and the small variant place
every vertex through:

```
X(u)   = 150 + 216 * (1.15u - 0.15u^2)
Y(u,v) = (256 - 22u) + (v - 0.5) * 190 * (1 - 0.34u) + 28 * (v - 0.5) * u * (1 - u)
```

with `u` running left to right and `v` top to bottom, both over 0 to 1. If you
need the grid at a different density, change the sample count, never the
function. A grid that warps differently is a different mark.

<br>

## Colour

| Role | Value | Where |
|---|---|---|
| Brand primary | **NX Violet `#7700FF`** | The hexagon, the bevel ramp, the grid's far end, every action. |
| Brand secondary | **Cyan `#00e5ff`** | The casing under the grid, the lit tile, the refracted inner lip. Nowhere else. |
| Ground | **True black `#000000`** | Every card, every social image, every dark surface. |
| Text | `#efeaff` body, `#9a8fc0` secondary | |

Three rules, and they are the ones people break:

1. **Violet dominates. Cyan is subordinate.** Cyan is light trapped inside the
   material: an edge, a live status, the tile being refreshed. It is never a
   surface, never a second brand colour, and never a fill large enough to
   compete with the violet. If a rendering reads as teal, the cyan is wrong.
2. **The ground is black, not dark grey and not dark violet.** Colour is light
   laid on top of black. On an OLED those pixels are off, which is the entire
   reason the violet reads as emitted rather than painted.
3. **One light source, upper left.** Every gradient, bevel and lit edge in these
   files agrees on it. A new asset that lights from anywhere else stops looking
   like the same object.

<br>

## Shape

**Angular, never rounded.** Radii stay in the 3 to 6px band. **Pills are
banned** outright, including status pills, tag pills and rounded buttons: they
read as a toy next to a faceted crystal. Perfect circles are reserved for status
dots and spinners. Badges and chips are sharp-cut rectangles with wide-tracked
uppercase micro-labels, not lozenges.

Clear space around the mark or the lockup is one hexagon half-width on every
side. Nothing crosses it.

<br>

## Type

The system UI stack, never a webfont:

```
system-ui, -apple-system, "Segoe UI", Roboto, "Noto Sans", Cantarell, sans-serif
```

Weight and letter-spacing do the branding. The wordmark is 600 weight with
slightly negative tracking; the micro-label under it is 600 weight, uppercase,
tracked out past `0.2em`. Version strings and any codec identifier (`nxvc`) set
in the monospace stack.

<br>

## Do not

- Do not put the mark on a mid-tone or a photograph. It wants black, or paper
  with the mono variant.
- Do not recolour the hexagon to signal state. State is chips and edges, not the
  identity.
- Do not add a glow, a drop shadow or an outline to the mark. The bevel and the
  refracted edge are already the lighting.
- Do not stretch, rotate or flat-top the hexagon. Pointy-top, always.
- Do not substitute the NX brand mark for a third party's identity, and do not
  put another project's name next to this lockup as though it were a joint mark.

<br>

## Regenerating

The SVGs are committed as final artwork and are edited directly. The PNGs are
derived:

```sh
cd brand
rsvg-convert -w 1280 -h 640 nx-warp-social.svg -o nx-warp-social.png
rsvg-convert -w 1000          nx-warp-logo.svg   -o nx-warp-logo.png
rsvg-convert -w 512           nx-warp-mark.svg   -o nx-warp-mark.png
rsvg-convert -w 128           nx-warp-mark.svg   -o nx-warp-mark-128.png
```

`inkscape --export-type=png` and `cairosvg` produce the same output. For raster
icon sets, derive each size from the **correct variant**: 16, 24 and 32px from
`nx-warp-mark-small.svg`, everything above from `nx-warp-mark.svg`.

<br>

## Licence

The brand assets in this directory are part of the repository and carry its
Apache-2.0 licence, with the ordinary trademark caveat: the licence covers the
files, not permission to present something as NX Warp when it is not. Forks
should keep the code and change the mark.

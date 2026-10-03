from pathlib import Path
import numpy as np
from PIL import Image, ImageOps

root = Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003')
image = Image.open(root / 'private-dark-s0-source.png').convert('RGB')
eye = ImageOps.fit(image, (2176, 2176), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
stereo = np.concatenate((np.asarray(eye), np.asarray(eye)), axis=1).astype(np.float64)
r, g, b = stereo[..., 0], stereo[..., 1], stereo[..., 2]
# BT.709 full-range Y'CbCr; round-to-nearest-even and clip to 8-bit.
y = np.rint(0.2126*r + 0.7152*g + 0.0722*b)
cb = np.rint(128.0 - 0.114572*r - 0.385428*g + 0.5*b)
cr = np.rint(128.0 + 0.5*r - 0.454153*g - 0.045847*b)
with (root / 'private-dark-native-stereo-444.yuv').open('wb') as f:
    for plane in (y, cb, cr):
        f.write(np.clip(plane, 0, 255).astype(np.uint8).tobytes())
print(f'source={image.width}x{image.height} eye={eye.width}x{eye.height} stereo={stereo.shape[1]}x{stereo.shape[0]} format=planar-444 bt709=full range rounding=numpy-rint bytes={(root / "private-dark-native-stereo-444.yuv").stat().st_size}')

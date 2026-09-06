# Gallery

Every measured result that made it into a decision gets a committed picture here.

An entry records the **date**, the **fixture**, the **settings**, the **number**
the picture exists to carry, and the **command** that reproduces it. The command
is the whole point: a chart nobody can regenerate is a claim, not a measurement.

Images live in `docs/assets/` and are named for the branch that produced them
(`atlasenc-*` for the GPU encoder's ATLAS work).

---

## atlasenc-decision-sweep.png — the coded-vector search is what decides whether the atlas pays

![ATLAS vs picture across head speed, with and without the coded-vector search](assets/atlasenc-decision-sweep.png)

* **Date** — 2026-09-06
* **Fixture** — `nx-scratch/atlasref/s{0.0,0.05,0.1,0.2,0.4,0.8,1.6,2.5}.yuv`,
  1088×1088, 16 frames. One rendered content (`md5 a325d144`, static world) with
  eight pose tracks; the head rotation rate is the only variable.
* **Settings** — QP 26, intra-period 180, rANS, `--ctx v3`, `--tab v2`,
  `--custom-tables`, one eye, 289 tiles. Two arms (picture model, `--atlas`) ×
  two decisions (with and without `--coded-vectors`).
* **Number** — **2.06×**. With the coded-vector search on, the atlas holds
  **86.5 % WARP_SKIP** and **half the picture model's bitrate at equal PSNR**
  (159505 B against 328754 B over 16 frames, −0.007 dB) at *every* speed from
  0.05 to 2.5 deg/frame — a factor of fifty in angular velocity with no change
  in the atlas column. It also codes **38.9 tiles a frame against 289**, 7.4×
  less Pass B at the decoder. With the search off, both models collapse to
  all-INTRA and the atlas's advantage vanishes; that is the regime every earlier
  GPU-encoder measurement was taken in, and it is why the reference's shape
  never reproduced.
* **Caveat** — `--coded-vectors` is *not* byte-identical to `nxv-enc`: E1c
  searches `STATIC_MV` only while the reference under `--int-coded-vectors on`
  also searches `WARP_MV`. Fast turn matched exactly; near-still differed by 94
  bytes in 159505. The configuration that makes the atlas pay is not yet the
  configuration the acid tests pin.
* **Command**

  ```sh
  # per speed, per arm; --atlas for the atlas arm, --coded-vectors for "cv on"
  nxvc-vkenc --in atlasref/s0.4.yuv --w 1088 --h 1088 --pix yuv420p --qp 26 \
      --frames 16 --nsub 3 --matrix 1 --wm 0 --tskip off --chroma-qp-off 0 \
      --ctx v3 --eyes 1 --intra-dir off --poses atlasref/s0.4.poses.json \
      --intra-period 180 --inter --custom-tables --tab v2 --device 0 \
      --coded-vectors --atlas --display-psnr --modes --out out.nxv
  ```

  Bytes from the output size, PSNR from `displayed PSNR-Y:`, skip % from the
  `--modes` census.

---

## atlasenc-tile-modes.png — what the two models decide, tile by tile, on one frame

![Per-tile decision maps for the picture model, ATLAS, and a PICTURE frame](assets/atlasenc-tile-modes.png)

* **Date** — 2026-09-06
* **Fixture** — `nx-scratch/atlasref/fastturn-adr.yuv` (71 deg/s mean),
  1088×1088, frame 8 of 16, 17×17 tiles.
* **Settings** — QP 26, intra-period 180, `--coded-vectors`, one eye. Three
  arms: picture model, `--atlas`, and `--atlas --atlas-mode` at the default
  `D = 8`.
* **Number** — **287 of 289 tiles skipped** under ATLAS on a frame where the
  picture model skips **none** and codes 288 `STATIC_MV` vectors. The
  per-frame-mode arm fires a PICTURE frame on this frame and reproduces the
  picture model's map exactly — 0 skip, 288 static — which is 13.12.11 working
  as specified and also why `D = 8` is the wrong default for this encoder: a
  PICTURE frame here throws away 287 free tiles.
* **Command**

  ```sh
  # once per arm; --tile-map writes frame,tile,row,col,eye,mode,picture
  nxvc-vkenc --in atlasref/fastturn-adr.yuv --w 1088 --h 1088 --pix yuv420p \
      --qp 26 --frames 16 --nsub 3 --matrix 1 --wm 0 --tskip off \
      --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off \
      --poses atlasref/fastturn-adr.poses.json --intra-period 180 --inter \
      --custom-tables --tab v2 --device 0 --coded-vectors \
      --atlas --atlas-mode --tile-map tf-mode.csv --out /dev/null
  ```

  `mode` is the nxvw value: 0 `WARP_SKIP`, 1 `STATIC_MV`, 2 `WARP_MV`, 3
  `INTRA`. `picture` is 1 on a frame coded as a PICTURE frame.

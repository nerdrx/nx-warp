# Paired Haar 4:2:0 offline gate

Encoded and decoded one native 4352x2176 stereo frame from the same source as the 4:4:4 checks. The original full-range BT.709 YUV 4:4:4 source was converted to planar 4:2:0 by preserving Y and box-averaging each 2x2 Cb/Cr block with `floor((sum + 2) / 4)`. This is deterministic nearest-integer averaging.

The private feature-enabled host Haar encoder used an exact 694,328-byte payload target and emitted a 694,260-byte packet (694,304-byte file including the 44-byte header). The feature-enabled host compute decoder read actual 4352x2176 Y and 2176x1088 Cb/Cr planes. Against the converted 4:2:0 source, plane MAE was Y 1.481642, Cb 1.310970, Cr 1.100527. `preview-420.png` compares source and actual decode after bilinear chroma upsampling for display; it is a small visual check, not headset validation.

The cross-container negatives both rejected: CDF fixture passed to the Haar helper and Haar fixture passed to the CDF helper each exited 1 with “bad input header or transform format.” This verifies their distinct magic/format gate.

Private reproducibility inputs, bitstream, readback planes, and encoder binary are retained under `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003/motion-visual/`. The modified host encoder source and preview calculator are included here.

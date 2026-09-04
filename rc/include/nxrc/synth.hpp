// Synthetic 64x64 luma tiles used by the tests and by nxvc-rcsim.  Kept in
// the library (not in tests/) so the simulator and the unit tests classify
// exactly the same material.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXRC_SYNTH_HPP
#define NXRC_SYNTH_HPP

#include <cstdint>
#include <vector>

namespace nxrc::synth {

// All generators fill a tile_size x tile_size buffer with stride tile_size.

// Smooth luma ramp: large variance, almost no gradient energy -> Flat.
void flat_gradient(uint8_t* out, int n, int lo = 40, int hi = 90);
// Constant tile.
void flat_const(uint8_t* out, int n, int value = 128);
// One straight step edge at angle `deg` -> Edge (high coherence).
void hard_edge(uint8_t* out, int n, float deg = 20.0f, int lo = 30, int hi = 220);
// Band-limited pseudo-random texture -> Texture (high activity, low coherence).
void noise_texture(uint8_t* out, int n, uint32_t seed = 1, int amp = 60,
                   int base = 128);
// A field of 5x7 glyphs, black on white, 1 px stems -> Text.
void text_glyphs(uint8_t* out, int n, uint32_t seed = 7);
// Strongly oriented high-contrast stripes: the statistical Text route
// (high coherence + high contrast + high activity).
void oriented_stripes(uint8_t* out, int n, float deg = 0.0f, int period = 4);

} // namespace nxrc::synth

#endif

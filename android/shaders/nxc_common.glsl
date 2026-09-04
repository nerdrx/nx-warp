// Shared decoding of the per-tile metadata word. Bindings and push constants are
// NOT shared: the decoder owns its descriptors and the HUD owns its own, which is
// the contract nxc_decoder.h commits to and the one the real vk/ decoder needs.
#ifndef NXC_COMMON_GLSL
#define NXC_COMMON_GLSL

// TRANSPORT.md 7.3: [15:0] pose_seq, [23:16] age, [25:24] state,
// [26] late, [27] recovered.
uint meta_pose(uint m)  { return m & 0xffffu; }
uint meta_age(uint m)   { return (m >> 16) & 0xffu; }
uint meta_state(uint m) { return (m >> 24) & 3u; }
uint meta_late(uint m)  { return (m >> 26) & 1u; }
uint meta_recov(uint m) { return (m >> 27) & 1u; }

const uint STATE_EMPTY       = 0u;
const uint STATE_DECODED     = 1u;
const uint STATE_CONCEALED   = 2u;
const uint STATE_UNDECODABLE = 3u;

#endif  // NXC_COMMON_GLSL

// ASCII and SVG "which tile is at which ladder step" maps.
// SPDX-License-Identifier: Apache-2.0
#ifndef NXRC_SIM_RENDER_HPP
#define NXRC_SIM_RENDER_HPP

#include "nxrc/nxrc.hpp"
#include "nxfov/foveation.hpp"

#include <span>
#include <string>
#include <vector>

namespace rcsim {

// One eye's worth of tiles, rendered as a character grid.
std::string ascii_step_map(const nxrc::AllocResult& a, int tiles_x, int tiles_y);
// Severity in QP-equivalent units, bucketed: '.' 0, '-' <=3, '+' <=9,
// '#' <=15, '@' above.  Comparable across classes, unlike the step index.
std::string ascii_sev_map(const nxrc::AllocResult& a, const nxfov::FoveationMap& f,
                          const nxrc::RateConfig& cfg, int tiles_x, int tiles_y);
std::string ascii_class_map(std::span<const uint8_t> cls, int tiles_x, int tiles_y);
std::string ascii_res_map(const nxrc::AllocResult& a, int tiles_x, int tiles_y);
std::string ascii_qp_map(const nxrc::AllocResult& a, int tiles_x, int tiles_y);
std::string ascii_fov_map(const nxfov::FoveationMap& f, int tiles_x, int tiles_y);

// Two maps side by side with a header line each.
std::string side_by_side(const std::string& a, const std::string& b,
                         const std::string& ta, const std::string& tb,
                         int gap = 4);

struct SvgPanel {
    std::string label;
    const nxrc::AllocResult* alloc;
    const std::vector<uint8_t>* cls;
    const nxfov::FoveationMap* fov;
};

// A row of panels, one per budget, coloured by ladder step with the tile
// class drawn as a glyph, plus a legend.
std::string svg_ladder_sheet(const std::vector<SvgPanel>& panels,
                             const nxrc::RateConfig& cfg,
                             int tiles_x, int tiles_y);

} // namespace rcsim

#endif

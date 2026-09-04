// SPDX-License-Identifier: Apache-2.0
#include "render.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>

namespace rcsim {

namespace {

std::string grid(int tiles_x, int tiles_y,
                 const std::function<char(size_t)>& ch) {
    std::string s;
    s.reserve(size_t(tiles_y) * size_t(tiles_x + 1));
    for (int y = 0; y < tiles_y; ++y) {
        for (int x = 0; x < tiles_x; ++x) s.push_back(ch(size_t(y) * tiles_x + x));
        s.push_back('\n');
    }
    return s;
}

} // namespace

std::string ascii_step_map(const nxrc::AllocResult& a, int tx, int ty) {
    static const char kC[] = {'.', '-', '+', '#', '@'};
    return grid(tx, ty, [&](size_t i) -> char {
        if (a.skip[i]) return ' ';
        return kC[std::min<int>(a.ladder_step[i], 4)];
    });
}

static int sev_bucket(float sev) {
    if (sev <= 0.01f) return 0;
    if (sev <= 3.0f)  return 1;
    if (sev <= 9.0f)  return 2;
    if (sev <= 15.0f) return 3;
    return 4;
}

std::string ascii_sev_map(const nxrc::AllocResult& a, const nxfov::FoveationMap& f,
                          const nxrc::RateConfig& cfg, int tx, int ty) {
    static const char kC[] = {'.', '-', '+', '#', '@'};
    return grid(tx, ty, [&](size_t i) -> char {
        if (a.skip[i]) return ' ';
        return kC[sev_bucket(nxrc::ladder_severity(a, f, i, cfg))];
    });
}

std::string ascii_class_map(std::span<const uint8_t> cls, int tx, int ty) {
    return grid(tx, ty, [&](size_t i) -> char {
        switch (cls[i]) {
            case uint8_t(nxrc::TileClass::Flat):    return '.';
            case uint8_t(nxrc::TileClass::Texture): return 'x';
            case uint8_t(nxrc::TileClass::Edge):    return 'E';
            case uint8_t(nxrc::TileClass::Text):    return 'T';
            default: return '?';
        }
    });
}

std::string ascii_res_map(const nxrc::AllocResult& a, int tx, int ty) {
    return grid(tx, ty, [&](size_t i) -> char {
        if (a.skip[i]) return ' ';
        if (a.dc_plane[i]) return 'D';
        return char('0' + std::min<int>(a.res_level[i], 2));
    });
}

std::string ascii_qp_map(const nxrc::AllocResult& a, int tx, int ty) {
    // One hex-ish digit per 4 QP steps: 0 = QP 0-3, f = QP 60+.
    static const char kC[] = "0123456789abcdef";
    return grid(tx, ty, [&](size_t i) -> char {
        if (a.skip[i]) return ' ';
        return kC[std::min<int>(a.qp[i] / 4, 15)];
    });
}

std::string ascii_fov_map(const nxfov::FoveationMap& f, int tx, int ty) {
    return grid(tx, ty, [&](size_t i) -> char {
        return char('0' + std::min<int>(f.level[i], 2));
    });
}

std::string side_by_side(const std::string& a, const std::string& b,
                         const std::string& ta, const std::string& tb, int gap) {
    std::vector<std::string> la, lb;
    {
        std::istringstream s(a); std::string l;
        while (std::getline(s, l)) la.push_back(l);
    }
    {
        std::istringstream s(b); std::string l;
        while (std::getline(s, l)) lb.push_back(l);
    }
    size_t wa = ta.size();
    for (auto& l : la) wa = std::max(wa, l.size());

    std::string out;
    out += ta + std::string(wa - ta.size() + gap, ' ') + tb + "\n";
    const size_t rows = std::max(la.size(), lb.size());
    for (size_t i = 0; i < rows; ++i) {
        const std::string& x = i < la.size() ? la[i] : std::string();
        out += x;
        out += std::string(wa - x.size() + gap, ' ');
        if (i < lb.size()) out += lb[i];
        out += "\n";
    }
    return out;
}

// ------------------------------------------------------------------ svg ---

std::string svg_ladder_sheet(const std::vector<SvgPanel>& panels,
                             const nxrc::RateConfig& cfg, int tx, int ty) {
    const int cell = 7, pad = 14, head = 26, legend = 62;
    const int pw = tx * cell, ph = ty * cell;
    const int W = pad + int(panels.size()) * (pw + pad);
    const int H = pad + head + ph + legend;

    // Ladder step colours: step 0 untouched .. step 4 DC plane.
    static const char* kStepCol[5] = {
        "#1d3f6e", "#2f6f9e", "#4fa3a0", "#c9a227", "#b3462f"
    };
    static const char* kStepName[5] = {
        "untouched", "low-pass matrix", "res 1/2", "res 1/4", "DC plane"
    };

    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W
      << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H
      << "\" font-family=\"ui-monospace,Menlo,Consolas,monospace\">\n";
    o << "<rect width=\"" << W << "\" height=\"" << H << "\" fill=\"#0d1117\"/>\n";

    for (size_t p = 0; p < panels.size(); ++p) {
        const int x0 = pad + int(p) * (pw + pad);
        const int y0 = pad + head;
        o << "<text x=\"" << x0 << "\" y=\"" << pad + 14
          << "\" fill=\"#c9d1d9\" font-size=\"12\">" << panels[p].label << "</text>\n";
        const nxrc::AllocResult& a = *panels[p].alloc;
        for (int y = 0; y < ty; ++y) {
            for (int x = 0; x < tx; ++x) {
                const size_t i = size_t(y) * tx + x;
                if (i >= a.size()) continue;
                const int px = x0 + x * cell, py = y0 + y * cell;
                if (a.skip[i]) {
                    o << "<rect x=\"" << px << "\" y=\"" << py << "\" width=\"" << cell - 1
                      << "\" height=\"" << cell - 1 << "\" fill=\"#161b22\"/>\n";
                    continue;
                }
                const int st = sev_bucket(
                    nxrc::ladder_severity(a, *panels[p].fov, i, cfg));
                o << "<rect x=\"" << px << "\" y=\"" << py << "\" width=\"" << cell - 1
                  << "\" height=\"" << cell - 1 << "\" fill=\"" << kStepCol[st] << "\"/>\n";
                // Class marker: text and edge tiles get a white pip so the
                // "structure held while texture blurred" reading is visual.
                const uint8_t c = (*panels[p].cls)[i];
                if (c == uint8_t(nxrc::TileClass::Text))
                    o << "<rect x=\"" << px + 2 << "\" y=\"" << py + 2
                      << "\" width=\"2\" height=\"2\" fill=\"#ffffff\"/>\n";
                else if (c == uint8_t(nxrc::TileClass::Edge))
                    o << "<rect x=\"" << px + 2 << "\" y=\"" << py + 2
                      << "\" width=\"2\" height=\"2\" fill=\"#8b949e\"/>\n";
            }
        }
    }

    int lx = pad, ly = pad + head + ph + 20;
    for (int s = 0; s < 5; ++s) {
        o << "<rect x=\"" << lx << "\" y=\"" << ly - 9 << "\" width=\"10\" height=\"10\" fill=\""
          << kStepCol[s] << "\"/>\n";
        o << "<text x=\"" << lx + 15 << "\" y=\"" << ly
          << "\" fill=\"#c9d1d9\" font-size=\"11\">" << kStepName[s] << "</text>\n";
        lx += 150;
    }
    o << "<text x=\"" << pad << "\" y=\"" << ly + 18
      << "\" fill=\"#8b949e\" font-size=\"11\">"
      << "white pip = text tile, grey pip = edge tile, dark cell = SKIP_WARP"
      << "</text>\n";
    o << "</svg>\n";
    return o.str();
}

} // namespace rcsim

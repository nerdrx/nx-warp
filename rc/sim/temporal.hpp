// The temporal-ladder scenario of nxvc-rcsim.  Writes RESULTS-temporal.md.
// SPDX-License-Identifier: Apache-2.0
#ifndef NXRC_SIM_TEMPORAL_HPP
#define NXRC_SIM_TEMPORAL_HPP

#include <string>

namespace rcsim {

// Runs the temporal scenario and writes `out_dir`/RESULTS-temporal.md and
// `out_dir`/refresh-map.svg.  Returns 0 on success.
int run_temporal(const std::string& out_dir, int frames, bool quiet);

} // namespace rcsim

#endif

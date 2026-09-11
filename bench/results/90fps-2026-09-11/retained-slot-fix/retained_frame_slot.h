// Retain the newest distinct frame IDs; gaps must not alias occupied slots.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <cstddef>

namespace wivrn {
inline std::optional<size_t> retained_frame_slot(std::span<const std::optional<uint64_t>> ids, uint64_t incoming)
{
 std::optional<size_t> empty, oldest;
 for (size_t i = 0; i < ids.size(); ++i) {
  if (ids[i] == incoming) return i;
  if (!ids[i]) { if (!empty) empty = i; }
  else if (!oldest || *ids[i] < *ids[*oldest]) oldest = i;
 }
 if (empty) return empty;
 if (oldest && incoming > *ids[*oldest]) return oldest;
 return std::nullopt;
}
}

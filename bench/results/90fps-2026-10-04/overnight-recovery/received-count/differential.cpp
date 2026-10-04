// Differential invariant check against the original completeness scan.
#include "shard_set.h"
#include <array>
#include <cstdio>
#include <random>
using S = wivrn::shard_set;
using D = S::data_shard;
bool oracle(const S &s) {
  const auto &d = s.shards();
  if (d.empty() || !d.back() || !d.back()->timing_info)
    return false;
  for (const auto &v : d)
    if (!v)
      return false;
  return true;
}
D make(uint64_t frame, uint16_t idx, uint16_t end) {
  D d{};
  d.frame_idx = frame;
  d.shard_idx = idx;
  if (idx == 0)
    d.view_info.emplace();
  if (idx + 1 == end)
    d.timing_info.emplace();
  return d;
}
int main() {
  std::mt19937 rng(20261004);
  std::array<S, 4> a;
  size_t checks = 0;
  for (unsigned n = 0; n < 50000; ++n) {
    auto i = rng() % a.size();
    auto j = rng() % a.size();
    auto &s = a[i];
    switch (rng() % 9) {
    case 0:
      s.reset(rng() % 1000);
      break;
    case 1:
    case 2:
    case 3: {
      auto end = uint16_t(1 + rng() % 521);
      auto idx = uint16_t(rng() % end);
      s.insert(make(s.frame_index(), idx, end), 1 + n);
      break;
    }
    case 4:
      a[i] = a[j];
      break;
    case 5:
      a[i] = std::move(a[j]);
      break;
    case 6: {
      S temp(a[j]);
      a[i] = std::move(temp);
      temp.insert(make(temp.frame_index(), 0, 1), 1 + n);
      if (!temp.complete())
        return 2;
      break;
    }
    case 7: {
      S p{};
      p.reset(7);
      p.insert(make(7, 0, 2), 1);
      S::parity_shard q{};
      q.first_shard_idx = 0;
      q.shard_stride = 1;
      q.blob_size = {0, 1};
      p.reconstruct(q, 2);
      if (p.complete() != oracle(p))
        return 3;
      a[i] = std::move(p);
      break;
    }
    case 8:
      s.insert(make(s.frame_index(), S::max_shards_per_frame, 0), 1 + n);
      break;
    }
    for (const auto &v : a) {
      ++checks;
      for (bool over : {false, true}) {
        std::vector<uint16_t> got, want;
        v.missing_shards(got, over);
        const auto &d = v.shards();
        for (size_t k = 0; k < d.size(); ++k)
          if (!d[k])
            want.push_back(uint16_t(k));
        if (over && !d.empty() && d.back() && !d.back()->timing_info &&
            d.size() < S::max_shards_per_frame)
          want.push_back(uint16_t(d.size()));
        ++checks;
        if (got != want)
          return 4;
      }
      if (v.complete() != oracle(v)) {
        std::fprintf(stderr, "mismatch after operation %u\n", n);
        return 1;
      }
    }
  }
  std::printf("%zu differential completeness/missing-list checks, 0 failures "
              "(50000 mutations; seed20261004)\n",
              checks);
}

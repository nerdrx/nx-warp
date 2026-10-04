#include "nxastc_packet_decode.h"
#include <lz4.h>
#include <zstd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
static std::vector<uint8_t> read_astc(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("cannot open input");
  auto n = f.tellg();
  if (n != 1183760)
    throw std::runtime_error("input must be 2176x2176 ASTC 8x8");
  std::vector<uint8_t> b(static_cast<size_t>(n), 0);
  f.seekg(0);
  f.read(reinterpret_cast<char *>(b.data()), n);
  const uint8_t h[16] = {0x13, 0xab, 0xa1, 0x5c, 8, 8, 1, 0x80,
                         0x08, 0,    0x80, 0x08, 0, 1, 0, 0};
  if (!f || std::memcmp(b.data(), h, 16))
    throw std::runtime_error("wrong ASTC header/block extent/image extent");
  return b;
}
static int pin_current_cpu() {
  cpu_set_t allowed, only;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    throw std::runtime_error("sched_getaffinity failed");
  int cpu = sched_getcpu();
  if (cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) {
    cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i)
      if (CPU_ISSET(i, &allowed)) {
        cpu = i;
        break;
      }
  }
  if (cpu < 0)
    throw std::runtime_error("no allowed CPU");
  CPU_ZERO(&only);
  CPU_SET(cpu, &only);
  if (sched_setaffinity(0, sizeof(only), &only) != 0 || sched_getcpu() != cpu)
    throw std::runtime_error("CPU pin verification failed");
  return cpu;
}
struct Input {
  std::vector<uint8_t> astc, raw;
};
struct Workspace {
  ZSTD_CCtx *ctx = ZSTD_createCCtx();
  std::vector<uint8_t> zbuf, lbuf, compact;
  Workspace() {
    if (!ctx)
      throw std::runtime_error("Zstd context allocation failed");
  }
  ~Workspace() { ZSTD_freeCCtx(ctx); }
};
struct Mode {
  const char *name;
  int level;
  bool compact;
};
static constexpr std::array<Mode, 4> modes{{{"ordinary_l3", 3, false},
                                            {"ordinary_l1", 1, false},
                                            {"compact_l1", 1, true},
                                            {"compact_l3", 3, true}}};
struct Result {
  std::vector<uint8_t> packet;
  wivrn::nxastc_packet::compression codec{};
  double compact_ms{}, zstd_ms{}, lz4_ms{}, selector_ms{}, pack_ms{};
  size_t compact_bytes{}, zstd_bytes{}, lz4_bytes{};
};
static Result encode(const Input &in, Workspace &w, const Mode &mode) {
  using namespace wivrn::nxastc_packet;
  const uint32_t raw_size = uint32_t(in.raw.size());
  Result r;
  auto all_begin = Clock::now();
  const uint8_t *zinput = in.raw.data();
  size_t zinput_size = in.raw.size();
  bool compact_input = false;
  if (mode.compact) {
    auto cbegin = Clock::now();
    size_t cbytes = 0;
    if (!compact_bytes_for_raw(raw_size, cbytes))
      throw std::runtime_error("invalid compact ASTC size");
    w.compact.resize(cbytes);
    compact_input = compact_blocks(in.raw, w.compact);
    r.compact_ms = ms(cbegin, Clock::now());
    if (!compact_input)
      throw std::runtime_error("production compact_blocks rejected q6 ASTC input");
    r.compact_bytes = cbytes;
    zinput = w.compact.data();
    zinput_size = cbytes;
  }
  auto zbegin = Clock::now();
  w.zbuf.resize(ZSTD_compressBound(zinput_size));
  r.zstd_bytes = ZSTD_compressCCtx(w.ctx, w.zbuf.data(), w.zbuf.size(),
                                   zinput, zinput_size, mode.level);
  r.zstd_ms = ms(zbegin, Clock::now());

  size_t fallback_size = raw_size;
  const uint8_t *fallback = in.raw.data();
  r.codec = compression::none;
  if (ZSTD_isError(r.zstd_bytes) || r.zstd_bytes == 0 ||
      r.zstd_bytes > raw_size / 2) {
    auto lbegin = Clock::now();
    w.lbuf.resize(size_t(LZ4_compressBound(int(raw_size))));
    const int n = LZ4_compress_default(
        reinterpret_cast<const char *>(in.raw.data()),
        reinterpret_cast<char *>(w.lbuf.data()), int(raw_size),
        int(w.lbuf.size()));
    r.lz4_ms = ms(lbegin, Clock::now());
    r.lz4_bytes = n > 0 ? size_t(n) : 0;
    if (r.lz4_bytes && r.lz4_bytes < raw_size) {
      fallback_size = r.lz4_bytes;
      fallback = w.lbuf.data();
      r.codec = compression::lz4;
    }
  }
  if (!ZSTD_isError(r.zstd_bytes) && r.zstd_bytes > 0 &&
      uint64_t(r.zstd_bytes) * 100 <= uint64_t(fallback_size) * 90 &&
      (!compact_input || r.zstd_bytes <= w.compact.size())) {
    fallback_size = r.zstd_bytes;
    fallback = w.zbuf.data();
    r.codec = compact_input ? compression::compact_zstd : compression::zstd;
  }
  r.selector_ms = ms(all_begin, Clock::now());
  auto pbegin = Clock::now();
  auto h = make_header(2176, 2176, uint32_t(fallback_size), r.codec);
  r.packet.resize(h.size() + fallback_size);
  std::memcpy(r.packet.data(), h.data(), h.size());
  std::memcpy(r.packet.data() + h.size(), fallback, fallback_size);
  r.pack_ms = ms(pbegin, Clock::now());
  return r;
}
static void verify(const Input &in, const std::vector<uint8_t> &packet) {
  using namespace wivrn::nxastc_packet;
  auto h = parse_packet(packet);
  if (!h || h->width != 2176 || h->height != 2176 ||
      h->raw_bytes != in.raw.size())
    throw std::runtime_error("NX packet parse failed");
  std::vector<uint8_t> decoded(in.raw.size());
  auto payload = std::span<const uint8_t>(packet).subspan(h->header_bytes,
                                                          h->payload_bytes);
  if (decode_payload(*h, payload, decoded) != decode_status::ok ||
      decoded != in.raw)
    throw std::runtime_error("production decode_payload mismatch");
}
struct Sample {
  int order{}, mode{}, iteration{}, cpu{};
  std::array<std::string, 2> codec;
  std::array<size_t, 2> packet_bytes{};
  std::array<double, 2> compact{}, zstd{}, lz4{}, selector{}, pack{};
};
static double pct(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[size_t((v.size() - 1) * p)];
}
static const char *codec_name(wivrn::nxastc_packet::compression c) {
  using C = wivrn::nxastc_packet::compression;
  return c == C::compact_zstd ? "compact_zstd"
       : c == C::zstd        ? "zstd"
       : c == C::lz4         ? "lz4"
                             : "raw";
}
int main(int argc, char **argv) try {
  if (argc != 4)
    throw std::runtime_error("usage: zstd_compact dark-q6.astc forest-q6.astc results.csv");
  Input dark{read_astc(argv[1]), {}}, forest{read_astc(argv[2]), {}};
  dark.raw.assign(dark.astc.begin() + 16, dark.astc.end());
  forest.raw.assign(forest.astc.begin() + 16, forest.astc.end());
  const int pinned = pin_current_cpu();
  std::array<Input, 2> inputs{std::move(dark), std::move(forest)};
  std::array<std::array<Workspace, 2>, 4> workspaces;
  std::array<std::array<std::vector<uint8_t>, 2>, 4> examples;
  std::array<std::array<bool, 2>, 4> captured{};
  std::vector<Sample> rows;
  rows.reserve(800);
  int order = 0;
  auto run = [&](int mode, int iteration, bool keep) {
    Sample s;
    s.order = order++;
    s.mode = mode;
    s.iteration = iteration;
    for (int eye = 0; eye < 2; ++eye) {
      auto r = encode(inputs[eye], workspaces[mode][eye], modes[mode]);
      s.codec[eye] = codec_name(r.codec);
      s.packet_bytes[eye] = r.packet.size();
      s.compact[eye] = r.compact_ms;
      s.zstd[eye] = r.zstd_ms;
      s.lz4[eye] = r.lz4_ms;
      s.selector[eye] = r.selector_ms;
      s.pack[eye] = r.pack_ms;
      if (!captured[mode][eye]) {
        examples[mode][eye] = std::move(r.packet);
        captured[mode][eye] = true;
      }
    }
    s.cpu = sched_getcpu();
    if (s.cpu != pinned)
      throw std::runtime_error("CPU affinity changed during sample");
    if (keep)
      rows.push_back(std::move(s));
  };
  constexpr int warmups = 20, measured = 200;
  for (int i = 0; i < warmups / 2; ++i)
    for (int m : {0, 1, 2, 3, 3, 2, 1, 0})
      run(m, i, false);
  for (int i = 0; i < measured / 2; ++i)
    for (int m : {0, 1, 2, 3, 3, 2, 1, 0})
      run(m, i, true);
  for (int m = 0; m < 4; ++m)
    for (int eye = 0; eye < 2; ++eye)
      verify(inputs[eye], examples[m][eye]);
  std::ofstream csv(argv[3]);
  if (!csv)
    throw std::runtime_error("cannot open CSV output");
  csv << "order,condition,iteration,pinned_cpu,eye0_codec,eye1_codec,eye0_packet_bytes,eye1_packet_bytes,eye0_compact_ms,eye1_compact_ms,eye0_zstd_ms,eye1_zstd_ms,eye0_lz4_ms,eye1_lz4_ms,eye0_selector_ms,eye1_selector_ms,eye0_pack_ms,eye1_pack_ms,two_eye_serial_cpu_ms,wire_250_mbps_ms,wire_500_mbps_ms\n";
  for (const auto &s : rows) {
    const size_t bytes = s.packet_bytes[0] + s.packet_bytes[1];
    const double cpu = s.selector[0] + s.selector[1] + s.pack[0] + s.pack[1];
    csv << s.order << ',' << modes[s.mode].name << ',' << s.iteration << ','
        << pinned << ',' << s.codec[0] << ',' << s.codec[1] << ','
        << s.packet_bytes[0] << ',' << s.packet_bytes[1] << ','
        << s.compact[0] << ',' << s.compact[1] << ',' << s.zstd[0] << ','
        << s.zstd[1] << ',' << s.lz4[0] << ',' << s.lz4[1] << ','
        << s.selector[0] << ',' << s.selector[1] << ',' << s.pack[0] << ','
        << s.pack[1] << ',' << cpu << ','
        << double(bytes) * 8.0 / 250e6 * 1000.0 << ','
        << double(bytes) * 8.0 / 500e6 * 1000.0 << '\n';
  }
  std::cout << "pinned_cpu=" << pinned << " measured_per_condition="
            << measured << " packet_decode_payload_exact=true\n";
  for (int m = 0; m < 4; ++m) {
    std::vector<double> cpu, z0, z1, c0, c1, w250, w500;
    size_t b0 = 0, b1 = 0, n = 0;
    std::array<unsigned, 3> codecs{};
    for (const auto &s : rows)
      if (s.mode == m) {
        cpu.push_back(s.selector[0] + s.selector[1] + s.pack[0] + s.pack[1]);
        z0.push_back(s.zstd[0]);
        z1.push_back(s.zstd[1]);
        c0.push_back(s.compact[0]);
        c1.push_back(s.compact[1]);
        b0 += s.packet_bytes[0];
        b1 += s.packet_bytes[1];
        const double wire = double(s.packet_bytes[0] + s.packet_bytes[1]) * 8.0 / 250e6 * 1000.0;
        w250.push_back(wire);
        w500.push_back(wire * .5);
        ++n;
        if (s.codec[0] == "compact_zstd" || s.codec[0] == "zstd") ++codecs[0];
        else if (s.codec[0] == "lz4") ++codecs[1];
        else ++codecs[2];
      }
    std::cout << modes[m].name << " cpu_pair=" << pct(cpu,.5) << '/' << pct(cpu,.95)
              << " zstd_dark=" << pct(z0,.5) << '/' << pct(z0,.95)
              << " zstd_forest=" << pct(z1,.5) << '/' << pct(z1,.95)
              << " compact_dark=" << pct(c0,.5) << '/' << pct(c0,.95)
              << " compact_forest=" << pct(c1,.5) << '/' << pct(c1,.95)
              << " mean_packet_bytes=" << double(b0)/n << '/' << double(b1)/n
              << " wire250=" << pct(w250,.5) << '/' << pct(w250,.95)
              << " wire500=" << pct(w500,.5) << '/' << pct(w500,.95)
              << " selected_zstd=" << codecs[0] << " lz4=" << codecs[1]
              << " raw=" << codecs[2] << '\n';
  }
  return 0;
} catch (const std::exception &e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}

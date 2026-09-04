// nxfuzz_main.cpp -- the regression runner.
//
// Linked into every <target>_replay binary.  It replays a corpus directory
// through LLVMFuzzerTestOneInput without libFuzzer, so a CI without clang
// still runs every checked-in reproducer on every push.  It also drives the
// target's LLVMFuzzerCustomMutator, so the mutator itself is covered (a
// mutator that reads out of bounds is a bug like any other, and historically
// the most common bug in a structure-aware harness).
//
// Usage:  <target>_replay [options] <file-or-dir>...
//   --timeout S    per-input wall clock limit, seconds (default 10, 0 = off)
//   --mutate N     after replaying, run N mutation rounds seeded from the
//                  corpus and execute each result (default 0)
//   --max-len N    cap for mutator output (default 65536)
//   --seed N       mutator seed (default 1)
//   --dump-dir DIR write each mutated input to DIR before executing it, so
//                  the last file written is the reproducer for a crash
//   --include-open also replay inputs under an `open/` directory.  Those are
//                  the reproducers for findings that are *not fixed yet*
//                  (fuzz/FINDINGS.md), so they still crash; they are skipped by
//                  default to keep the CTest entry green, and this flag is how
//                  you reproduce every open finding at once.
//   --quiet        only print the summary
//
// A missing directory is not an error: the fuzz corpora are generated and a
// fresh checkout may legitimately have an empty regressions/ directory.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#define NXFUZZ_HAVE_ALARM 1
#endif

#include "nxfuzz.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Optional in the target.
extern "C" __attribute__((weak)) size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size,
                                                                size_t max_size,
                                                                unsigned seed);
extern "C" __attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv);

// ---------------------------------------------------------------------------
// Deterministic stand-in for libFuzzer's mutator.  It is intentionally simple:
// its job is to keep the custom mutator's byte-level fallback path alive under
// a non-clang build, not to find bugs on its own.
// ---------------------------------------------------------------------------
namespace {
uint64_t g_mut_state = 0x243F6A8885A308D3ull;
uint64_t mut_next() {
    uint64_t z = (g_mut_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
}  // namespace

extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size) {
    if (max_size == 0) return 0;
    if (size > max_size) size = max_size;
    switch (mut_next() % 4) {
        case 0:  // flip a byte
            if (size) data[mut_next() % size] ^= uint8_t(1u << (mut_next() % 8));
            return size;
        case 1: {  // grow
            size_t add = 1 + static_cast<size_t>(mut_next() % 16);
            size_t n = std::min(max_size, size + add);
            for (size_t i = size; i < n; ++i) data[i] = uint8_t(mut_next());
            return n;
        }
        case 2:  // shrink
            return size ? size - 1 : 0;
        default:  // splash a byte
            if (size) data[mut_next() % size] = uint8_t(mut_next());
            return size;
    }
}

// ---------------------------------------------------------------------------
namespace {

const char *g_current = "";
double g_timeout = 10.0;
bool g_quiet = false;

#ifdef NXFUZZ_HAVE_ALARM
void on_alarm(int) {
    // async-signal-safe enough for a crash path
    static const char msg[] = "\n=== TIMEOUT: input took longer than the limit: ";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    r = write(2, g_current, std::strlen(g_current));
    r = write(2, "\n", 1);
    (void)r;
    _exit(70);
}
#endif

bool g_include_open = false;

// An input under a directory called `open/` reproduces a finding that has not
// been fixed, so replaying it aborts on purpose.  See fuzz/FINDINGS.md.
bool is_open_input(const std::filesystem::path &p) {
    for (const auto &part : p) {
        if (part == "open") return true;
    }
    return false;
}

bool read_file(const std::filesystem::path &p, std::vector<uint8_t> &out) {
    std::FILE *f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void collect(const std::filesystem::path &p, std::vector<std::filesystem::path> &out) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        if (!g_quiet) std::fprintf(stderr, "note: %s does not exist, skipping\n", p.string().c_str());
        return;
    }
    if (std::filesystem::is_directory(p, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator(p, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (!g_include_open && is_open_input(it->path())) continue;
            out.push_back(it->path());
        }
    } else {
        // An explicitly named file is always run, `open/` or not: that is how a
        // single reproducer is replayed by hand.
        out.push_back(p);
    }
}

void run_one(const std::vector<uint8_t> &buf, const std::string &name) {
    g_current = name.c_str();
#ifdef NXFUZZ_HAVE_ALARM
    if (g_timeout > 0) alarm((unsigned)(g_timeout + 0.999));
#endif
    auto t0 = std::chrono::steady_clock::now();
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
#ifdef NXFUZZ_HAVE_ALARM
    alarm(0);
#endif
    if (g_timeout > 0 && dt > g_timeout) {
        std::fprintf(stderr, "=== TIMEOUT: %s took %.2fs\n", name.c_str(), dt);
        std::exit(70);
    }
    if (!g_quiet && dt > 0.5)
        std::fprintf(stderr, "slow: %.2fs %s\n", dt, name.c_str());
}

}  // namespace

int main(int argc, char **argv) {
    if (LLVMFuzzerInitialize) LLVMFuzzerInitialize(&argc, &argv);
#ifdef NXFUZZ_HAVE_ALARM
    std::signal(SIGALRM, on_alarm);
#endif

    size_t mutate_rounds = 0, max_len = 65536;
    unsigned seed = 1;
    std::string dump_dir;
    std::vector<std::filesystem::path> roots;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char *) -> const char * {
            return (i + 1 < argc) ? argv[++i] : "0";
        };
        if (a == "--timeout") g_timeout = std::atof(val("t"));
        else if (a == "--mutate") mutate_rounds = (size_t)std::strtoull(val("m"), nullptr, 10);
        else if (a == "--max-len") max_len = (size_t)std::strtoull(val("l"), nullptr, 10);
        else if (a == "--seed") seed = (unsigned)std::strtoul(val("s"), nullptr, 10);
        else if (a == "--dump-dir") dump_dir = val("d");
        else if (a == "--include-open") g_include_open = true;
        else if (a == "--quiet") g_quiet = true;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: %s [--timeout S] [--mutate N] [--max-len N] "
                        "[--seed N] [--dump-dir DIR] [--include-open] [--quiet] "
                        "<file-or-dir>...\n", argv[0]);
            return 0;
        } else roots.push_back(a);
    }

    std::vector<std::filesystem::path> files;
    for (const auto &r : roots) collect(r, files);
    std::sort(files.begin(), files.end());

    std::vector<uint8_t> buf;
    size_t ran = 0, bytes = 0;
    for (const auto &f : files) {
        if (!read_file(f, buf)) continue;
        bytes += buf.size();
        run_one(buf, f.string());
        ++ran;
    }

    // The empty input must always be safe; every target gets it for free.
    buf.clear();
    run_one(buf, "<empty>");

    size_t mutated = 0;
    if (mutate_rounds && LLVMFuzzerCustomMutator) {
        std::vector<uint8_t> work(max_len);
        nxf::Rng rng(seed ? seed : 1);
        for (size_t i = 0; i < mutate_rounds; ++i) {
            std::vector<uint8_t> base;
            if (!files.empty()) {
                const auto &f = files[rng.below((uint32_t)files.size())];
                if (!read_file(f, base)) base.clear();
            }
            if (base.size() > max_len) base.resize(max_len);
            std::fill(work.begin(), work.end(), 0);
            if (!base.empty()) std::memcpy(work.data(), base.data(), base.size());
            size_t n = LLVMFuzzerCustomMutator(work.data(), base.size(), max_len,
                                               (unsigned)(seed + i));
            if (n > max_len) {
                std::fprintf(stderr,
                             "=== BUG: LLVMFuzzerCustomMutator returned %zu > max_size %zu\n",
                             n, max_len);
                return 71;
            }
            std::vector<uint8_t> out(work.begin(), work.begin() + n);
            // --dump-dir writes the input *before* running it, so after a
            // crash the last file in the directory is the reproducer.  That is
            // how the inputs under fuzz/regressions/ were captured without a
            // libFuzzer build.
            if (!dump_dir.empty()) {
                char name[64];
                std::snprintf(name, sizeof name, "/mut-%08zu.bin", i);
                std::string path = dump_dir + name;
                if (std::FILE *df = std::fopen(path.c_str(), "wb")) {
                    if (!out.empty()) std::fwrite(out.data(), 1, out.size(), df);
                    std::fclose(df);
                }
            }
            run_one(out, "<mutated>");
            ++mutated;
        }
    } else if (mutate_rounds) {
        std::fprintf(stderr, "note: target has no LLVMFuzzerCustomMutator, --mutate ignored\n");
    }

    std::printf("nxfuzz replay: %zu corpus input(s), %zu byte(s), %zu mutation(s)%s -- OK\n",
                ran, bytes, mutated, g_include_open ? " (including open/)" : "");
    return 0;
}

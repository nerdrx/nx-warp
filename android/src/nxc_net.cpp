#include "nxc_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nxc {

// ---------------------------------------------------------------- /proc helpers

long clock_ticks_per_sec() {
    static const long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

uint64_t read_cpu_ticks(int tid) {
    char path[64];
    if (tid == 0) {
        snprintf(path, sizeof(path), "/proc/self/stat");
    } else {
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    }
    FILE* f = fopen(path, "re");
    if (!f) return UINT64_MAX;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return UINT64_MAX;
    buf[n] = 0;
    // Field 2 is the comm, in parentheses, and may itself contain spaces and
    // parentheses -- so scan from the LAST ')' rather than tokenising from the
    // start. utime and stime are fields 14 and 15, i.e. 11 and 12 after ')'.
    char* p = strrchr(buf, ')');
    if (!p) return UINT64_MAX;
    ++p;
    int field = 2;
    unsigned long long utime = 0, stime = 0;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        ++field;
        char* end = nullptr;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) { while (*p && *p != ' ') ++p; continue; }
        if (field == 14) utime = v;
        if (field == 15) { stime = v; return utime + stime; }
        p = end;
    }
    return UINT64_MAX;
}

Receiver::SnmpUdp Receiver::read_snmp_udp() {
    SnmpUdp out;
    FILE* f = fopen("/proc/net/snmp", "re");
    if (!f) return out;
    char line[1024];
    std::vector<std::string> hdr;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Udp:", 4) != 0) continue;
        if (hdr.empty()) {
            hdr.clear();
            char* save = nullptr;
            for (char* t = strtok_r(line, " \t\n", &save); t; t = strtok_r(nullptr, " \t\n", &save))
                hdr.push_back(t);
            continue;
        }
        char* save = nullptr;
        size_t i = 0;
        for (char* t = strtok_r(line, " \t\n", &save); t; t = strtok_r(nullptr, " \t\n", &save), ++i) {
            if (i >= hdr.size()) break;
            if (hdr[i] == "InErrors")      out.in_errors     = strtoull(t, nullptr, 10);
            else if (hdr[i] == "RcvbufErrors") out.rcvbuf_errors = strtoull(t, nullptr, 10);
        }
        out.ok = true;
        break;
    }
    fclose(f);
    return out;
}

int pick_fastest_cpu(uint64_t* out_khz) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return -1;

    int best = -1;
    uint64_t best_khz = 0;
    const int ncpu = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    for (int c = 0; c < ncpu && c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &allowed)) continue;
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE* f = fopen(path, "re");
        if (!f) continue;
        char buf[32] = {};
        uint64_t khz = 0;
        if (fgets(buf, sizeof(buf), f)) khz = strtoull(buf, nullptr, 10);
        fclose(f);
        // Strictly greater: on a big.LITTLE part with two identical prime cores
        // this picks the lower-numbered one, which is the conventional choice.
        if (khz > best_khz) { best_khz = khz; best = c; }
    }
    if (out_khz) *out_khz = best_khz;
    return best;
}

// ---------------------------------------------------------------- Receiver

Receiver::Receiver(const AppConfig& cfg) : cfg_(cfg) {
    ring_ = std::make_unique<SpscRing>(cfg.ring_slots, cfg.slot_bytes);
}

Receiver::~Receiver() {
    stop();
    if (fd_ >= 0) close(fd_);
}

bool Receiver::open() {
    if (!ring_ || !ring_->valid()) {
        log_err("receive ring allocation failed (%u slots x %u bytes)",
                cfg_.ring_slots, cfg_.slot_bytes);
        return false;
    }

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { log_err("socket: %s", strerror(errno)); return false; }

    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // ---- SO_RCVBUF (PAPER 4.11). SO_RCVBUFFORCE bypasses rmem_max but needs
    // CAP_NET_ADMIN, which an app does not have; try it anyway because it costs
    // one syscall and some ROMs are permissive, then fall back.
    caps_.rcvbuf_requested = cfg_.want_rcvbuf;
    int want = cfg_.want_rcvbuf;
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof(want)) == 0) {
        caps_.rcvbuf_forced = true;
    } else if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) != 0) {
        caps_.notes += "SO_RCVBUF failed: " + std::string(strerror(errno)) + "; ";
        // Walk down until something is accepted, so a capped ROM still gets the
        // largest buffer it will give rather than the 200 KB default.
        for (int w = want / 2; w >= 256 * 1024; w /= 2) {
            if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &w, sizeof(w)) == 0) break;
        }
    }
    socklen_t sl = sizeof(caps_.rcvbuf_granted);
    if (getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &caps_.rcvbuf_granted, &sl) != 0)
        caps_.rcvbuf_granted = -1;

    // Linux reports double the value it stores (the extra half is the sk_buff
    // accounting overhead allowance), so the usable payload capacity is half.
    log_info("SO_RCVBUF requested %d, getsockopt reports %d (usable ~%d, forced=%d)",
             caps_.rcvbuf_requested, caps_.rcvbuf_granted,
             caps_.rcvbuf_granted > 0 ? caps_.rcvbuf_granted / 2 : -1,
             int(caps_.rcvbuf_forced));
    if (caps_.rcvbuf_granted > 0 && caps_.rcvbuf_granted / 2 < cfg_.want_rcvbuf) {
        log_warn("ROM capped SO_RCVBUF at %d bytes usable; PAPER 4.11 says lower "
                 "the burst cap when this happens", caps_.rcvbuf_granted / 2);
    }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(cfg_.listen_port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        log_err("bind port %u: %s", cfg_.listen_port, strerror(errno));
        close(fd_); fd_ = -1;
        return false;
    }
    log_info("listening on udp/%u", cfg_.listen_port);
    return true;
}

void Receiver::apply_thread_tuning() {
    // ---- affinity to the fastest core the ROM allows.
    if (cfg_.want_affinity) {
        uint64_t khz = 0;
        int cpu = pick_fastest_cpu(&khz);
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) == 0) {
                caps_.chosen_cpu = cpu;
                caps_.chosen_cpu_khz = khz;
                caps_.affinity_mask = 1u << cpu;
                log_info("receive thread pinned to cpu%d (%llu kHz)",
                         cpu, (unsigned long long)khz);
            } else {
                caps_.notes += "sched_setaffinity: " + std::string(strerror(errno)) + "; ";
                log_warn("sched_setaffinity(cpu%d) refused: %s", cpu, strerror(errno));
            }
        } else {
            caps_.notes += "cpufreq unreadable, affinity left alone; ";
            log_warn("cpufreq not readable, leaving receive-thread affinity alone");
        }
    }

    // ---- SCHED_FIFO, gracefully.
    if (cfg_.want_sched_fifo) {
        sched_param p{};
        p.sched_priority = cfg_.sched_fifo_prio;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
        if (rc == 0) {
            caps_.sched_fifo = true;
            log_info("receive thread is SCHED_FIFO prio %d", p.sched_priority);
        } else {
            caps_.notes += "SCHED_FIFO: " + std::string(strerror(rc)) + "; ";
            log_warn("SCHED_FIFO refused (%s); falling back to SCHED_OTHER with "
                     "nice -19, which is what an unprivileged app gets", strerror(rc));
            // Best effort: a strong negative nice usually fails too without
            // CAP_SYS_NICE, but Android grants some headroom to the foreground app.
            setpriority(PRIO_PROCESS, 0, -19);
        }
    }
    sched_param cur{};
    int pol = 0;
    if (pthread_getschedparam(pthread_self(), &pol, &cur) == 0) {
        caps_.sched_policy = pol;
        caps_.sched_priority = cur.sched_priority;
    }
}

void Receiver::start() {
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread([this] { thread_main(); });
}

void Receiver::stop() {
    if (!running_.exchange(false)) return;
    // Unblock recvmmsg: shutdown() is enough on Linux for a bound UDP socket to
    // return from a blocking receive.
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
}

void Receiver::thread_main() {
    pthread_setname_np(pthread_self(), "nxc-rx");
    apply_thread_tuning();
    rx_tid_.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_relaxed);

    const uint32_t batch = uint32_t(cfg_.recv_batch);
    std::vector<mmsghdr> msgs(batch);
    std::vector<iovec>   iov(batch);
    std::vector<sockaddr_storage> addrs(batch);

    // Scratch used only to drain the socket when the ring is full: the kernel
    // buffer must never be the thing that backs up, otherwise the loss we
    // measure is our own scheduling and not the receive path's ceiling.
    std::vector<uint8_t> scratch(size_t(batch) * cfg_.slot_bytes);

    while (running_.load(std::memory_order_relaxed)) {
        uint32_t n = ring_->claim(batch);

        uint8_t*  base = nullptr;
        uint32_t* lens = nullptr;
        SpscRing::SrcAddr* srcs = nullptr;
        bool to_ring = n > 0;
        if (to_ring) {
            base = ring_->claim_data();
            lens = ring_->claim_lens();
            srcs = ring_->claim_srcs();
        } else {
            n = batch;
            base = scratch.data();
        }

        for (uint32_t i = 0; i < n; ++i) {
            iov[i].iov_base = base + size_t(i) * cfg_.slot_bytes;
            iov[i].iov_len  = cfg_.slot_bytes;
            memset(&msgs[i].msg_hdr, 0, sizeof(msghdr));
            msgs[i].msg_hdr.msg_iov     = &iov[i];
            msgs[i].msg_hdr.msg_iovlen  = 1;
            msgs[i].msg_hdr.msg_name    = &addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
            msgs[i].msg_len = 0;
        }

        // MSG_WAITFORONE: block until at least one datagram, then take whatever
        // else is already queued. This is the batching behaviour the pps ceiling
        // depends on -- without it recvmmsg degrades to one syscall per datagram
        // at low rates and to a full-batch spin at high rates.
        int got = recvmmsg(fd_, msgs.data(), n, MSG_WAITFORONE, nullptr);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (!running_.load(std::memory_order_relaxed)) break;
            stats_.recv_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (got == 0) continue;

        stats_.wakeups.fetch_add(1, std::memory_order_relaxed);
        stats_.batches.fetch_add(1, std::memory_order_relaxed);
        stats_.batch_msgs.fetch_add(uint32_t(got), std::memory_order_relaxed);

        if (!to_ring) {
            // Ring was full: these datagrams are gone. Counted separately from
            // network loss so a self-test run can tell the two apart.
            stats_.drops_ring_full.fetch_add(uint32_t(got), std::memory_order_relaxed);
            continue;
        }

        uint64_t bytes = 0;
        for (int i = 0; i < got; ++i) {
            lens[i] = msgs[i].msg_len;
            bytes += msgs[i].msg_len;
            uint32_t alen = msgs[i].msg_hdr.msg_namelen;
            if (alen > sizeof(srcs[i].bytes)) alen = sizeof(srcs[i].bytes);
            memcpy(srcs[i].bytes, &addrs[i], alen);
            srcs[i].len = alen;
        }
        // Learn the peer once per batch rather than once per datagram.
        if (peer_len_.load(std::memory_order_relaxed) == 0) {
            uint32_t alen = msgs[0].msg_hdr.msg_namelen;
            if (alen > sizeof(peer_addr_)) alen = sizeof(peer_addr_);
            memcpy(peer_addr_, &addrs[0], alen);
            peer_len_.store(alen, std::memory_order_release);
            char ip[INET6_ADDRSTRLEN] = "?";
            const auto* sin = reinterpret_cast<const sockaddr_in*>(peer_addr_);
            if (sin->sin_family == AF_INET)
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            log_info("peer learned: %s:%u", ip, ntohs(sin->sin_port));
        }

        ring_->publish(uint32_t(got));
        stats_.datagrams.fetch_add(uint32_t(got), std::memory_order_relaxed);
        stats_.bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    log_info("receive thread exiting");
}

bool Receiver::peer(void* out_sockaddr, uint32_t* out_len) const {
    uint32_t l = peer_len_.load(std::memory_order_acquire);
    if (l == 0) return false;
    memcpy(out_sockaddr, peer_addr_, l);
    *out_len = l;
    return true;
}

bool Receiver::send_to_peer(const void* data, size_t len) {
    uint32_t l = peer_len_.load(std::memory_order_acquire);
    if (l == 0 || fd_ < 0) return false;
    ssize_t s = sendto(fd_, data, len, MSG_DONTWAIT,
                       reinterpret_cast<const sockaddr*>(peer_addr_), l);
    return s == ssize_t(len);
}

}  // namespace nxc

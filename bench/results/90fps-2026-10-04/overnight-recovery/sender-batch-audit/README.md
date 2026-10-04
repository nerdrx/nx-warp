# ASTC sender batching audit

## Prior work checked

`sender-overlap/README.md` measured sender-wait placement with a simulated FIFO
sender. It found mixed results and made no network or packet-syscall claim. The
reassembly and repair notes concern receive retirement/NACK behavior, not sender
batching. No prior report measured production UDP send syscall cost.

## Current path

Encoded frame jobs enter `video_encoder::sender::push()` and its bounded pending
frame queue (8 jobs); `sender::run()` drains one frame job at a time and calls
`SendData()` ([server/encoder/video_encoder.h:93];
[server/encoder/video_encoder.cpp:84-120, 182-201]). `SendData()` shards the
encoded span at the UDP payload budget, snapshots recovery/FEC blobs when
needed, then calls `wivrn_connection::send_stream()` once per primary data
shard ([server/encoder/video_encoder.cpp:822-899, 918-932]). It updates the
FEC builder and sends parity immediately when a block closes; each parity shard
also calls `send_stream()` separately ([server/encoder/video_encoder.cpp:780-809,
940-954]). Frame history copies only primary/UDP shards, after send, and is
bounded separately ([server/encoder/video_encoder.cpp:956-966]).

For primary UDP, `typed_socket::send(T&&)` reuses one thread-local
`serialization_packet` and calls `UDP::send_raw()`
([common/wivrn_sockets.h:374-380]). `send_raw()` reuses thread-local iovec
storage and calls `writev_with_retry()` once normally per datagram
([common/wivrn_sockets.cpp:476-503]); that retries `EINTR`, `EAGAIN`,
`EWOULDBLOCK`, and `ENOBUFS` up to 64 times with 60 us backoff
([common/wivrn_sockets.cpp:67-91]). AES-CTR uses a fresh atomic IV counter per
shard. At 500/1000 Mbit/s of payload and 1400 bytes per datagram, this is about
44,600/89,300 data datagrams per second, plus FEC parity when enabled.

Pacing is byte-based, not syscall-based. `shard_pacer` groups about 12 KiB
(roughly 8-9 datagrams) between sleep points and skips sleeps below 150 us
([server/encoder/shard_pacer.h:45-63, 127-145]). The sender shares its pacing
slot across streams ([server/encoder/shard_pacer.h:155-193]). Striping sends a
prefix over UDP and suffix over TCP; parity remains on UDP and is emitted at FEC
block boundaries ([server/encoder/video_encoder.cpp:901-954]). Buffering data
until a batch is full must therefore preserve FEC parity order, flush before
sleep, and not hold a shard across the primary/secondary split. Whole-frame or
cross-eye batching is not a small change.

The retransmit path is simpler: the network thread collects up to 64 recovered
shards from history, then sends each immediately and unpaced through
`connection->send_stream()` ([server/driver/wivrn_session.cpp:1040-1065];
[server/encoder/video_encoder.h:316-320]). The configured cap is 2000
retransmitted shards/s. Replies are already collected before the send loop, so
one batch of at most 64 would add no intentional wait and has no shard-pacer
boundary to preserve.

## Existing batching and decision

The generic socket API already has `send(std::span<serialization_packet>)` and
UDP `send_many_raw()`, whose thread-local iovec, `mmsghdr`, and IV-counter
vectors are reused; `sendmmsg()` sends the packets
([common/wivrn_sockets.h:382-384]; [common/wivrn_sockets.cpp:506-561]). No
production call site for that overload exists. Only the TCP desynchronization
test directly exercises `send_many_raw()` ([tests/tcp_desync_test.cpp:190-220]).
Single-packet sends already reuse their serializer and iovec storage, so batching
would save syscalls, not all per-packet construction or encryption work.

**Candidate for a small experiment:** batch only the already-collected NACK
reply vector, capped at 64 datagrams. This avoids data-frame pacing, eye order,
FEC group ordering, and added wait. It is still only a candidate: current UDP
`send_many_raw()` does not retry transient errors and does not detect a positive
partial `sendmmsg()` result; it checks only for a negative result, then reports
all bytes as sent ([common/wivrn_sockets.cpp:558-561]). That is weaker than the
single-shard retry path and could turn one pressured send into a larger repair
loss. Do not wire it into production unless partial/error behavior is made
explicit and a test confirms it.

Do not batch the high-rate `SendData()` path based only on packet rate. Current
pacing already sends microbursts; the 12 KiB boundary, FEC parity insertion,
striping split, encryption, and per-datagram retry/drop behavior all need to
remain equivalent. No sender CPU or syscall trace shows these calls are a
bottleneck today.

## Smallest future localhost check

Use production `UDP` typed sockets bound to loopback, and compare 1/8/64
serialized native video shards sent individually versus one `send_many_raw()`
batch. Use matching payloads, encryption keys, and frame IDs. Drain the peer,
then verify datagram count, order, decryption, and every payload byte. Measure
CPU time per request and first-to-last receiver arrival; do not call this Wi-Fi
latency. Repeat with a deliberately small local socket send buffer to trigger
partial/error cases. Require batch path to deliver or explicitly report every
unsent suffix, and to match single-send recovery semantics before considering a
production experiment. No GPU, headset, server-session, or network route is
needed.

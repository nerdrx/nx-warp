# Shard-history lookup cost

## Result

This is a host replay, not a stream/network measurement. It constructs three
synthetic frames at the 1 Gbit/s aggregate, 90 Hz stereo payload budget
(694,444 bytes per eye per frame), encoding every shard with production
`fec::encode_blob()`. It feeds identical blobs into exact 1 MiB and 2 MiB
`shard_history` snapshots.

Both histories return the same 64 current-frame shards, and the harness decodes
them and checks every payload byte. Both return zero for absent frame ID 9. The
larger history retains 1,563 entries versus 783 in the 1 MiB history. Each timed
sample repeats `collect()` 250 times; five matched ABBA blocks alternate
1M-2M-2M-1M and 2M-1M-1M-2M. Output contains loop means only.

| Lookup | 1 MiB mean | 2 MiB mean | Difference |
| --- | ---: | ---: | ---: |
| Current frame, 64 hits | 1.897 us | 2.435 us | +0.538 us (+28%) |
| Absent frame, miss | 0.285 us | 0.997 us | +0.712 us (3.5x baseline) |

The hit measurement includes `collect()`'s normal 64 blob allocations and
copies (~85 kB per call); both capacities return identical bytes. The delta
therefore measures additional work within this operation, not total
NACK processing. Miss path has no returned blobs. No per-sample tail statistic
or allocation counter was collected. The observed mean increase is under 1 us per collection call here;
this is not a worst-case bound or a full NACK measurement. The 1 MiB version scans fewer entries
because its byte ring has already evicted older entries.

The 1 MiB baseline is
`history-budget-20261004/baseline/shard_history.h`. The 2 MiB version is
`wt-pyrowave-probe/server/encoder/shard_history.h` at source commit
`6d8970d2184a31d82946bfb3c8ab09a7515f0938`. Current snapshot also returns
`vector::capacity()` from `bytes()` and explicitly assigns an empty vector when
disabling; these setup/reporting differences are outside timed `collect()`.

## Reproduce

Compile `lookup.cpp` with C++23/O2 using the same configured source/common, source/external, configured-build/common and Boost PFR include paths as the parent retention probe; link source/common/smp.cpp and libcrypto. The public include paths now select the exact sibling `../baseline` and `../candidate` header snapshots. Run `nice -n 5 taskset -c 3 ./lookup` only after confirming CPU3 is allowed. No system clock settings are changed.

The original measurement used GCC16.2.1/O2, nice+5 and CPU3 from allowed mask0–31. One process includes both exact headers under distinct class names. CSV rows are loop means; no individual-call tails or allocator instrumentation. The reported differences are observed component deltas, not a worst-case bound for the entire NACK path.

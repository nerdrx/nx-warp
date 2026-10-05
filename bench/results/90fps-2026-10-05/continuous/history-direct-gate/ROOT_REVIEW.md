# Root review

Root reviewed the scratch header diff against source 831aafed, bounded span accumulation, ring ownership, reader/publication ordering, and 60 retained timing rows. All seven recorded build/run exit files contain 0. Build and correctness logs are empty, consistent with quiet successful checks; the retained timing summary and raw rows agree.

Root independently compiled the **published** direct_gate.cpp and sibling headers with GCC -O2, the same configured source includes, common/smp.cpp and libcrypto, then ran normal correctness once. Combined command exited 0 on 2026-10-05. The binary stayed in scratch. No timed rerun or sanitizer rerun was required for copying unchanged source files into the report.

The concurrent constant-payload check is a decoding/ownership check, not an identity oracle. There is no network send in this harness, so early history publication remains an explicit integration mismatch. Rejection applies to this shortcut and measured narrow path; it does not establish that all history contention is negligible.

This harness uses upstream LZ4 v1.10.0 (`lz4.c` and `lz4.h`), identified by
the version macros in `lz4.h`. The upstream `LICENSE` is preserved alongside
the source. Files were copied from the host's cached `nx_lz4-src` dependency;
that cache did not contain Git metadata, so no commit hash is available.

Builds can use another explicit LZ4 source directory with
`ASTC_PICO_LZ4_DIR=/path/to/lz4/lib ./build.sh`.

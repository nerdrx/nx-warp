#pragma once

#include "lz4.h"

#include <climits>
#include <cstddef>

inline int nx_lz4_decompress_payload(const void* compressed, size_t compressed_size,
                                    void* output, size_t output_capacity) {
    if (compressed_size > INT_MAX || output_capacity > INT_MAX) return -1;
    return LZ4_decompress_safe(static_cast<const char*>(compressed),
        static_cast<char*>(output), static_cast<int>(compressed_size),
        static_cast<int>(output_capacity));
}

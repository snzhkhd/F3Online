// Native decoder for Granny 2.x "Oodle0" section compression (compression id 1).
// Reconstructed from granny2.dll (see docs/formats/granny.md). Dependency-free C++17.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gr2 {

// src: compressed section bytes (srcSize bytes; the decoder may read a few bytes of look-ahead, which are
// treated as zeros past srcSize). stop0/stop1: GR2 section header fields first16bit / first8bit,
// outSize: decompressed size. out must have outSize bytes. Returns false on malformed input.
bool oodle0Decompress(const uint8_t* src, size_t srcSize, uint32_t stop0, uint32_t stop1,
                      uint32_t outSize, uint8_t* out);

}  // namespace gr2

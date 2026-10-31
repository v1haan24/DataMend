#pragma once
#include <cstdint>
#include <vector>

typedef std::vector<uint8_t> Bytes;

// Count how many times each byte value (0-255) occurs.
std::vector<uint32_t> countFrequencies(const Bytes& data);

// Huffman-encode data. The tree is rebuilt from `freq`, so only freq must be stored.
Bytes huffmanCompress(const Bytes& data, const std::vector<uint32_t>& freq);

// Decode `packed` back into originalSize bytes.
Bytes huffmanDecompress(const Bytes& packed, const std::vector<uint32_t>& freq, uint64_t originalSize);

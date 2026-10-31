#pragma once
#include "huffman.h"

// Split data into 4 equal blocks (zero-padded). blockSize receives the block length.
std::vector<Bytes> splitIntoBlocks(const Bytes& data, size_t& blockSize);

// P = B1 ^ B2 ^ B3 ^ B4
Bytes makeParity(const std::vector<Bytes>& blocks);

// Mark one block as unavailable (works on a copy, never on the archive file).
void simulateLoss(std::vector<Bytes>& blocks, int missing);

// Rebuild the missing block from parity and the three surviving blocks.
Bytes recoverBlock(const std::vector<Bytes>& blocks, const Bytes& parity, int missing);

// Join blocks and drop the padding.
Bytes joinBlocks(const std::vector<Bytes>& blocks, size_t compressedSize);

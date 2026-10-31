#include "recovery.h"
using namespace std;

vector<Bytes> splitIntoBlocks(const Bytes& data, size_t& blockSize) {
    blockSize = (data.size() + 3) / 4;
    vector<Bytes> blocks(4, Bytes(blockSize, 0));
    for (size_t i = 0; i < data.size(); i++) blocks[i / blockSize][i % blockSize] = data[i];
    return blocks;
}

Bytes makeParity(const vector<Bytes>& blocks) {
    Bytes parity(blocks[0].size(), 0);
    for (size_t b = 0; b < blocks.size(); b++)
        for (size_t i = 0; i < parity.size(); i++) parity[i] ^= blocks[b][i];
    return parity;
}

void simulateLoss(vector<Bytes>& blocks, int missing) {
    blocks[missing].clear();
}

// B_missing = P ^ (all other blocks), so the lost block's data is never read.
Bytes recoverBlock(const vector<Bytes>& blocks, const Bytes& parity, int missing) {
    Bytes rebuilt = parity;
    for (int b = 0; b < 4; b++) {
        if (b == missing) continue;
        for (size_t i = 0; i < rebuilt.size(); i++) rebuilt[i] ^= blocks[b][i];
    }
    return rebuilt;
}

Bytes joinBlocks(const vector<Bytes>& blocks, size_t compressedSize) {
    Bytes data;
    for (size_t b = 0; b < blocks.size(); b++) data.insert(data.end(), blocks[b].begin(), blocks[b].end());
    data.resize(compressedSize);
    return data;
}

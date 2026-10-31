#include "huffman.h"
#include <functional>
#include <queue>
#include <string>
using namespace std;

namespace {

// Tree nodes live in a vector and refer to each other by index.
struct Node { int left = -1, right = -1, sym = -1; };

// Builds the Huffman tree with a min priority queue ordered by (frequency, node id).
// The id tie-break makes the tree identical every time, so decompression can rebuild it.
int buildTree(const vector<uint32_t>& freq, vector<Node>& tree) {
    typedef pair<uint64_t, int> Item;
    priority_queue<Item, vector<Item>, greater<Item> > pq;
    for (int s = 0; s < 256; s++) {
        if (freq[s] == 0) continue;
        Node leaf; leaf.sym = s;
        tree.push_back(leaf);
        pq.push(Item(freq[s], (int)tree.size() - 1));
    }
    if (pq.empty()) return -1;                      // empty file
    while (pq.size() > 1) {                         // merge the two rarest nodes
        Item a = pq.top(); pq.pop();
        Item b = pq.top(); pq.pop();
        Node parent; parent.left = a.second; parent.right = b.second;
        tree.push_back(parent);
        pq.push(Item(a.first + b.first, (int)tree.size() - 1));
    }
    return pq.top().second;
}

// Left edge = '0', right edge = '1'. A lone symbol gets the code "0".
void makeCodes(const vector<Node>& tree, int i, const string& code, vector<string>& codes) {
    if (tree[i].sym >= 0) { codes[tree[i].sym] = code.empty() ? "0" : code; return; }
    makeCodes(tree, tree[i].left, code + "0", codes);
    makeCodes(tree, tree[i].right, code + "1", codes);
}

} // namespace

vector<uint32_t> countFrequencies(const Bytes& data) {
    vector<uint32_t> freq(256, 0);
    for (size_t i = 0; i < data.size(); i++) freq[data[i]]++;
    return freq;
}

Bytes huffmanCompress(const Bytes& data, const vector<uint32_t>& freq) {
    Bytes out;
    vector<Node> tree;
    int root = buildTree(freq, tree);
    if (root < 0) return out;
    vector<string> codes(256);
    makeCodes(tree, root, "", codes);

    uint8_t cur = 0; int bits = 0;                  // pack code bits into bytes
    for (size_t i = 0; i < data.size(); i++) {
        const string& c = codes[data[i]];
        for (size_t k = 0; k < c.size(); k++) {
            cur = (uint8_t)((cur << 1) | (c[k] == '1'));
            if (++bits == 8) { out.push_back(cur); cur = 0; bits = 0; }
        }
    }
    if (bits > 0) out.push_back((uint8_t)(cur << (8 - bits)));
    return out;
}

Bytes huffmanDecompress(const Bytes& packed, const vector<uint32_t>& freq, uint64_t originalSize) {
    Bytes out;
    vector<Node> tree;
    int root = buildTree(freq, tree);
    if (root < 0) return out;
    out.reserve((size_t)originalSize);
    if (tree[root].sym >= 0) {                      // file made of one repeated byte
        out.assign((size_t)originalSize, (uint8_t)tree[root].sym);
        return out;
    }
    int cur = root;
    for (size_t i = 0; i < packed.size() * 8 && out.size() < originalSize; i++) {
        bool bit = (packed[i / 8] >> (7 - i % 8)) & 1;
        cur = bit ? tree[cur].right : tree[cur].left;
        if (tree[cur].sym >= 0) { out.push_back((uint8_t)tree[cur].sym); cur = root; }
    }
    return out;
}

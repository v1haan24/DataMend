#include "file_handler.h"
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
using namespace std;

// Archive layout (little-endian):
// "DMB1" | fileCount(4) | file record * fileCount
// file record:
//   nameLen(2) name | originalSize(8) | fingerprint(8) | compressedSize(8) | blockSize(8)
//   | symbolCount(2) | (byte(1) freq(4)) * symbolCount | block1..block4 | parity

static void put(Bytes& o, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

static uint64_t get(const Bytes& b, size_t& p, int n) {
    if (p + n > b.size()) throw runtime_error("corrupt archive");
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)b[p + i] << (8 * i);
    p += n;
    return v;
}

bool readFile(const string& path, Bytes& out) {
    ifstream f(path.c_str(), ios::binary);
    if (!f) return false;
    out.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
    return true;
}

bool writeFile(const string& path, const Bytes& data) {
    ofstream f(path.c_str(), ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), (streamsize)data.size());
    return (bool)f;
}

// FNV-1a 64-bit hash
uint64_t fnv1a(const Bytes& data) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < data.size(); i++) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}

static string lowered(string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

// Safe path: '\' becomes '/', ':' becomes '_', and empty, "." and ".." parts are dropped, so "../x" or "/abs" cannot escape a folder.
// Duplicates become "name (2).ext", "name (3).ext", ... (compared case-insensitively, like Windows).
int sanitizeNames(Archive& a) {
    set<string> used;
    int renamed = 0;
    for (size_t i = 0; i < a.files.size(); i++) {
        const string raw = a.files[i].name;
        string path, part;
        for (size_t k = 0; k <= raw.size(); k++) {
            char c = (k < raw.size()) ? raw[k] : '/';
            if (c == '\\') c = '/';
            if (c == ':') c = '_';                          // no drive letters like C:
            if (c == '/') {
                if (!part.empty() && part != "." && part != "..") path += (path.empty() ? "" : "/") + part;
                part.clear();
            } else if ((unsigned char)c >= 32) {
                part += c;
            }
        }
        if (path.empty()) path = "file";
        size_t slash = path.rfind('/'), dot = path.rfind('.');
        size_t start = (slash == string::npos) ? 0 : slash + 1;
        size_t cut = (dot == string::npos || dot <= start) ? path.size() : dot;   // keep the extension
        string name = path;
        for (int n = 2; used.count(lowered(name)); n++)
            name = path.substr(0, cut) + " (" + to_string(n) + ")" + path.substr(cut);
        if (name != path) renamed++;
        used.insert(lowered(name));
        a.files[i].name = name;
    }
    return renamed;
}

Bytes serializeArchive(const Archive& a) {
    Bytes o;
    put(o, 0x31424D44, 4);                                  // "DMB1"
    put(o, a.files.size(), 4);
    for (size_t k = 0; k < a.files.size(); k++) {
        const FileRecord& f = a.files[k];
        put(o, f.name.size(), 2);
        o.insert(o.end(), f.name.begin(), f.name.end());
        put(o, f.originalSize, 8); put(o, f.fingerprint, 8);
        put(o, f.compressedSize, 8); put(o, f.blockSize, 8);
        uint64_t count = 0;
        for (int s = 0; s < 256; s++) if (f.freq[s]) count++;
        put(o, count, 2);
        for (int s = 0; s < 256; s++) if (f.freq[s]) { put(o, s, 1); put(o, f.freq[s], 4); }
        for (size_t b = 0; b < f.blocks.size(); b++) o.insert(o.end(), f.blocks[b].begin(), f.blocks[b].end());
        o.insert(o.end(), f.parity.begin(), f.parity.end());
    }
    return o;
}

bool parseArchive(const Bytes& b, Archive& out) {
    Archive a;
    try {
        size_t p = 0;
        if (get(b, p, 4) != 0x31424D44) return false;
        size_t count = (size_t)get(b, p, 4);
        if (count == 0 || count > MAX_FILES) return false;
        uint64_t totalOriginal = 0;
        for (size_t k = 0; k < count; k++) {
            FileRecord f;
            size_t n = (size_t)get(b, p, 2);
            if (p + n > b.size()) return false;
            f.name.assign(b.begin() + p, b.begin() + p + n);
            p += n;
            f.originalSize = get(b, p, 8); f.fingerprint = get(b, p, 8);
            f.compressedSize = get(b, p, 8); f.blockSize = get(b, p, 8);
            if (f.blockSize > b.size()) return false;
            f.freq.assign(256, 0);
            size_t symbols = (size_t)get(b, p, 2);
            if (symbols > 256) return false;                // at most 256 distinct byte values
            for (size_t i = 0; i < symbols; i++) {
                size_t s = (size_t)get(b, p, 1);
                f.freq[s] = (uint32_t)get(b, p, 4);
            }
            uint64_t total = 0;
            for (int s = 0; s < 256; s++) total += f.freq[s];
            if (total != f.originalSize) return false;      // frequencies must add up to the file size
            if (f.compressedSize > 4 * f.blockSize || f.compressedSize + 3 < 4 * f.blockSize) return false;
            if (f.originalSize > 8 * f.compressedSize) return false;   // every Huffman code is at least 1 bit
            totalOriginal += f.originalSize;
            if (totalOriginal > (1ULL << 30)) return false; // refuse archives that expand beyond 1 GB
            size_t bs = (size_t)f.blockSize;
            if (p + 5 * bs > b.size()) return false;
            f.blocks.assign(4, Bytes());
            for (int i = 0; i < 4; i++) { f.blocks[i].assign(b.begin() + p, b.begin() + p + bs); p += bs; }
            f.parity.assign(b.begin() + p, b.begin() + p + bs);
            p += bs;
            a.files.push_back(f);
        }
        if (p != b.size()) return false;                    // nothing may follow the last record
    } catch (const exception&) {
        return false;
    }
    sanitizeNames(a);
    out.files.swap(a.files);
    return true;
}

bool saveArchive(const string& path, const Archive& a, uint64_t* fileSize) {
    Bytes o = serializeArchive(a);
    if (fileSize) *fileSize = o.size();
    return writeFile(path, o);
}

bool loadArchive(const string& path, Archive& a) {
    Bytes b;
    return readFile(path, b) && parseArchive(b, a);
}

// ---------- ZIP (store only) ----------
static uint32_t crc32(const Bytes& d) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < d.size(); i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Bytes makeZip(const vector<pair<string, Bytes> >& files) {
    Bytes out, central;
    for (size_t i = 0; i < files.size(); i++) {
        const string& name = files[i].first;
        const Bytes& data = files[i].second;
        uint32_t crc = crc32(data), size = (uint32_t)data.size(), offset = (uint32_t)out.size();
        // local file header: version 20, flag 0x0800 (UTF-8 names), method 0 (stored), date 1980-01-01
        put(out, 0x04034B50, 4); put(out, 20, 2); put(out, 0x0800, 2); put(out, 0, 2);
        put(out, 0, 2); put(out, 0x21, 2); put(out, crc, 4); put(out, size, 4); put(out, size, 4);
        put(out, name.size(), 2); put(out, 0, 2);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        // central directory entry
        put(central, 0x02014B50, 4); put(central, 20, 2); put(central, 20, 2); put(central, 0x0800, 2); put(central, 0, 2);
        put(central, 0, 2); put(central, 0x21, 2); put(central, crc, 4); put(central, size, 4); put(central, size, 4);
        put(central, name.size(), 2); put(central, 0, 2); put(central, 0, 2); put(central, 0, 2);
        put(central, 0, 2); put(central, 0, 4); put(central, offset, 4);
        central.insert(central.end(), name.begin(), name.end());
    }
    uint32_t cdOffset = (uint32_t)out.size();
    out.insert(out.end(), central.begin(), central.end());
    put(out, 0x06054B50, 4); put(out, 0, 2); put(out, 0, 2);
    put(out, files.size(), 2); put(out, files.size(), 2);
    put(out, central.size(), 4); put(out, cdOffset, 4); put(out, 0, 2);
    return out;
}

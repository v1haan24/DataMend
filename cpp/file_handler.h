#pragma once
#include <string>
#include <utility>
#include "huffman.h"

const size_t MAX_FILES = 1000;

// One backed-up file. Every record is self-contained, so each file recovers independently.
struct FileRecord {
    std::string name;                 // relative path, e.g. "notes.txt" or "docs/a.txt"
    uint64_t originalSize = 0;
    uint64_t fingerprint = 0;         // FNV-1a hash of the original file
    uint64_t compressedSize = 0;      // Huffman data size before padding
    uint64_t blockSize = 0;
    std::vector<uint32_t> freq;       // this file's Huffman metadata (256 byte frequencies)
    std::vector<Bytes> blocks;        // 4 data blocks
    Bytes parity;                     // XOR parity block (one per file)
};

// A .dmb backup: a list of file records. Original files are NOT stored.
struct Archive { std::vector<FileRecord> files; };

bool readFile(const std::string& path, Bytes& out);
bool writeFile(const std::string& path, const Bytes& data);
uint64_t fnv1a(const Bytes& data);

// Makes every name a safe relative path and unique (case-insensitive). Returns how many were renamed as duplicates.
int sanitizeNames(Archive& a);

Bytes serializeArchive(const Archive& a);
bool parseArchive(const Bytes& bytes, Archive& a);   // validates everything; false if malformed
bool saveArchive(const std::string& path, const Archive& a, uint64_t* fileSize = 0);
bool loadArchive(const std::string& path, Archive& a);

// Builds a ZIP (stored, no compression) from {path, contents} pairs.
Bytes makeZip(const std::vector<std::pair<std::string, Bytes> >& files);

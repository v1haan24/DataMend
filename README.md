# DataMend – Multi-File Compression & Recovery System

A small DSA project. Select many files, compress each one with Huffman coding, split each into four blocks, protect each with an XOR parity block, and store everything in **one `.dmb` archive**. Any single file can later be restored on its own, or all files together as a ZIP.

Workflow: Select files → Compress → Create Backup → Simulate Data Loss → Recover → Download Restored File / Download All.

## Features
- Multi-file selection (browse or drag and drop, folders included) with per-file remove and total size
- One combined `.dmb` archive; original files are not stored
- Each file has its own Huffman table, four data blocks, parity block and FNV-1a fingerprint, so files recover independently
- Duplicate names are renamed safely (`a.txt`, `a (2).txt`); unsafe paths (`../x`, `C:\x`) are neutralised
- Recovery screen: open any `.dmb`, see every file's status, simulate losing one block of a file, rebuild it, verify it
- Download one restored file, or **Download All** as a ZIP that keeps names and folders
- Reported sizes: original, Huffman-compressed, final backup (including parity and metadata)

## Archive format (`.dmb`, little-endian)
```
"DMB1" | fileCount (4) | file record x fileCount

file record:
  nameLen (2) | name | originalSize (8) | fingerprint (8) | compressedSize (8) | blockSize (8)
  symbolCount (2) | (byte (1), frequency (4)) x symbolCount
  block 1 | block 2 | block 3 | block 4 | parity      (each blockSize bytes)
```
The reader validates the magic number, file count, symbol count, that frequencies add up to the file size, that block size matches the compressed size, and that the file ends exactly after the last record. Anything else is rejected.

## Recovery and its limit
For each file: `P = B1 ⊕ B2 ⊕ B3 ⊕ B4`, so a lost block is `Bk = P ⊕ (the other three)`.
One parity block can rebuild **one** missing block **per file**. Different files can each lose one block and all be recovered, but two missing blocks in the same file cannot be recovered. Damaged (not missing) data is detected by the fingerprint check, not repaired. FNV-1a detects accidental corruption; it is not a cryptographic or tamper-proof hash.

## DSA concepts
Greedy algorithm and min priority queue (Huffman tree) · binary tree traversal · frequency table · bit manipulation and XOR parity · hashing (FNV-1a, CRC-32 for ZIP) · sets for unique names.

## Technology
C++17, STL and native OS socket APIs (Winsock on Windows, POSIX sockets on Linux) for a tiny local HTTP server; HTML, CSS and vanilla JavaScript for the UI. No compression libraries: Huffman coding and the ZIP writer (stored, uncompressed entries) are written from scratch, so there are no extra dependencies.

## Structure
```
DataMend/
  cpp/
    main.cpp          entry point, local HTTP server, API, compress/restore workflow
    huffman.*         frequencies, tree, codes, compress/decompress (per file)
    recovery.*        4-way split, XOR parity, loss simulation, rebuild (per file)
    file_handler.*    file I/O, .dmb archive, name sanitising, FNV-1a, ZIP writer
  web/                index.html, style.css, script.js
```

## Limits
About 190 MB and 1000 files per backup; everything is processed in memory.

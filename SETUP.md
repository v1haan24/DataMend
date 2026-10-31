# Setup

**Required:** g++ (MinGW-w64 / MSYS2, C++17) and a web browser.

**Open a terminal in the `DataMend` folder** (the one containing `cpp` and `web`).

**Compile:**
```
g++ -std=c++17 -O2 cpp/main.cpp cpp/huffman.cpp cpp/recovery.cpp cpp/file_handler.cpp -o datamend.exe -lws2_32
```

**Run:**
```
.\datamend.exe
```

**Open:** http://localhost:8080

**Stop:** press `Ctrl+C` in the terminal.

The last backup is saved as `backup.dmb` in this folder.

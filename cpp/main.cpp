// DataMend - local HTTP server that connects the web page to the C++ engine.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include "file_handler.h"
#include "huffman.h"
#include "recovery.h"

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCK (-1)
#define CLOSE_SOCK close
#endif
using namespace std;

const int PORT = 8080;
const string ARCHIVE = "backup.dmb";                        // last backup created on the Home screen

enum { INTACT, LOST, RESTORED, FAILED };
struct FileState { int missing = -1; int status = INTACT; };  // per file: simulated lost block + result

static Archive g_arch;                                      // archive currently open in Recovery
static vector<FileState> g_state;
static Bytes g_zip;                                         // result of the last "Download All"

struct Request { string method, path, query; Bytes body; };

// ---------- tiny HTTP helpers ----------
static bool readRequest(Sock s, Request& r) {
    static char buf[65536];
    string head;
    size_t end = string::npos;
    while (end == string::npos) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0 || head.size() > 131072) return false;
        head.append(buf, n);
        end = head.find("\r\n\r\n");
    }
    r.body.assign(head.begin() + end + 4, head.end());
    head.resize(end);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == string::npos || sp2 == string::npos) return false;
    r.method = head.substr(0, sp1);
    string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = (q == string::npos) ? "" : target.substr(q + 1);

    string low = head;
    for (size_t i = 0; i < low.size(); i++) low[i] = (char)tolower((unsigned char)low[i]);
    size_t cl = low.find("content-length:");
    size_t need = (cl == string::npos) ? 0 : (size_t)strtoull(low.c_str() + cl + 15, 0, 10);
    if (need > (200u << 20)) return false;                  // 200 MB upload limit
    while (r.body.size() < need) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) return false;
        r.body.insert(r.body.end(), buf, buf + n);
    }
    return true;
}

static void sendAll(Sock s, const char* d, size_t n) {
    while (n > 0) {
        int k = send(s, d, (int)(n < (1u << 20) ? n : (1u << 20)), 0);
        if (k <= 0) return;
        d += k; n -= k;
    }
}

static void respond(Sock s, int code, const string& type, const Bytes& body) {
    string h = "HTTP/1.1 " + to_string(code) + (code == 200 ? " OK" : " Error") +
               "\r\nContent-Type: " + type + "\r\nContent-Length: " + to_string(body.size()) +
               "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    sendAll(s, h.data(), h.size());
    sendAll(s, (const char*)body.data(), body.size());
}

static void respondText(Sock s, int code, const string& type, const string& text) {
    respond(s, code, type, Bytes(text.begin(), text.end()));
}

static string jsonStr(const string& v) {
    string o = "\"";
    for (size_t i = 0; i < v.size(); i++) {
        if ((unsigned char)v[i] < 32) continue;
        if (v[i] == '"' || v[i] == '\\') o += '\\';
        o += v[i];
    }
    return o + "\"";
}

static void json(Sock s, const string& body) { respondText(s, 200, "application/json; charset=utf-8", body); }
static void fail(Sock s, int code, const string& msg) {
    respondText(s, code, "application/json; charset=utf-8", "{\"error\":" + jsonStr(msg) + "}");
}

static string urlDecode(const string& v) {
    string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '%' && i + 2 < v.size()) { o += (char)strtol(v.substr(i + 1, 2).c_str(), 0, 16); i += 2; }
        else o += v[i];
    }
    return o;
}

static string getParam(const string& query, const string& key) {
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find('&', p);
        if (e == string::npos) e = query.size();
        string kv = query.substr(p, e - p);
        size_t eq = kv.find('=');
        if (eq != string::npos && kv.substr(0, eq) == key) return urlDecode(kv.substr(eq + 1));
        p = e + 1;
    }
    return "";
}

static void serveFile(Sock s, const string& path, const string& type) {
    Bytes data;
    if (!readFile(path, data)) return fail(s, 404, "File not found: " + path);
    respond(s, 200, type, data);
}

// ---------- operations (reuse huffman / recovery / file_handler) ----------
static void useArchive(Archive& a) {
    g_arch.files.swap(a.files);
    g_state.assign(g_arch.files.size(), FileState());
    g_zip.clear();
}

// Compress one file independently: own frequency table, 4 blocks, own parity block.
static FileRecord makeRecord(const string& name, const Bytes& data) {
    FileRecord f;
    f.name = name;
    f.originalSize = data.size();
    f.fingerprint = fnv1a(data);
    f.freq = countFrequencies(data);
    Bytes packed = huffmanCompress(data, f.freq);
    f.compressedSize = packed.size();
    size_t bs;
    f.blocks = splitIntoBlocks(packed, bs);
    f.blockSize = bs;
    f.parity = makeParity(f.blocks);
    return f;
}

// Restore one file. If a block is lost (missing >= 0) it is rebuilt from the parity block first.
static bool restoreFile(const FileRecord& f, int missing, Bytes& out, uint64_t& fp) {
    vector<Bytes> blocks = f.blocks;                        // work on a copy; the archive stays intact
    if (missing >= 0) {
        simulateLoss(blocks, missing);
        blocks[missing] = recoverBlock(blocks, f.parity, missing);
    }
    Bytes packed = joinBlocks(blocks, (size_t)f.compressedSize);
    out = huffmanDecompress(packed, f.freq, f.originalSize);
    fp = fnv1a(out);
    return out.size() == f.originalSize && fp == f.fingerprint;
}

// Upload format from the web page: count(4) | { nameLen(4) name size(8) data } * count
struct Upload { string name; size_t offset, size; };

static bool parseBundle(const Bytes& b, vector<Upload>& ups) {
    size_t p = 0;
    uint64_t count, len, size;
    auto num = [&](int n, uint64_t& v) -> bool {
        if (p + n > b.size()) return false;
        v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)b[p + i] << (8 * i);
        p += n;
        return true;
    };
    if (!num(4, count) || count > MAX_FILES) return false;
    for (uint64_t i = 0; i < count; i++) {
        if (!num(4, len) || len > 4096 || p + len > b.size()) return false;
        Upload u;
        u.name.assign(b.begin() + p, b.begin() + p + len);
        p += len;
        if (!num(8, size) || size > b.size() - p) return false;
        u.offset = p; u.size = (size_t)size;
        p += (size_t)size;
        ups.push_back(u);
    }
    return p == b.size();
}

static int getIndex(const string& query, const string& key, size_t limit) {
    string v = getParam(query, key);
    if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != string::npos) return -1;
    size_t n = (size_t)atoi(v.c_str());
    return n < limit ? (int)n : -1;
}

static string filesJson() {
    if (g_arch.files.empty()) return "{\"ready\":false,\"files\":[]}";
    static const char* status[] = { "intact", "lost", "restored", "failed" };
    string o = "{\"ready\":true,\"files\":[";
    for (size_t i = 0; i < g_arch.files.size(); i++) {
        const FileRecord& f = g_arch.files[i];
        if (i) o += ",";
        o += "{\"name\":" + jsonStr(f.name) + ",\"originalSize\":" + to_string(f.originalSize) +
             ",\"compressedSize\":" + to_string(f.compressedSize) + ",\"blockSize\":" + to_string(f.blockSize) +
             ",\"missing\":" + to_string(g_state[i].missing) + ",\"status\":\"" + status[g_state[i].status] + "\"}";
    }
    return o + "]}";
}

// ---------- API ----------
static void apiCompress(Sock s, const Request& r) {
    vector<Upload> ups;
    if (!parseBundle(r.body, ups)) return fail(s, 400, "Malformed upload (or a file name longer than 4096 bytes).");
    if (ups.empty()) return fail(s, 400, "Select at least one file.");
    Archive a;
    for (size_t i = 0; i < ups.size(); i++) {
        Bytes data(r.body.begin() + ups[i].offset, r.body.begin() + ups[i].offset + ups[i].size);
        a.files.push_back(makeRecord(ups[i].name, data));
        Bytes check; uint64_t fp;                           // verify the compressed record before trusting it
        if (!restoreFile(a.files.back(), -1, check, fp)) return fail(s, 500, "Verification failed for " + ups[i].name);
    }
    int renamed = sanitizeNames(a);
    uint64_t original = 0, compressed = 0, archiveSize = 0;
    for (size_t i = 0; i < a.files.size(); i++) { original += a.files[i].originalSize; compressed += a.files[i].compressedSize; }
    if (!saveArchive(ARCHIVE, a, &archiveSize)) return fail(s, 500, "Could not write " + ARCHIVE);
    size_t n = a.files.size();
    useArchive(a);
    json(s, "{\"fileCount\":" + to_string(n) + ",\"originalSize\":" + to_string(original) +
            ",\"compressedSize\":" + to_string(compressed) + ",\"archiveSize\":" + to_string(archiveSize) +
            ",\"renamed\":" + to_string(renamed) + "}");
}

static void apiOpen(Sock s, const Request& r) {
    Archive a;
    if (!parseArchive(r.body, a)) return fail(s, 400, "Not a valid DataMend archive (wrong format or damaged).");
    useArchive(a);
    json(s, filesJson());
}

static void apiSimulate(Sock s, const Request& r) {
    int f = getIndex(r.query, "file", g_arch.files.size()), b = getIndex(r.query, "block", 4);
    if (f < 0) return fail(s, 400, "Open an archive and choose a file first.");
    if (b < 0) return fail(s, 400, "Choose one data block (1-4).");
    g_state[f].missing = b;                                 // only one block per file can be lost
    g_state[f].status = LOST;
    json(s, filesJson());
}

static void apiRecover(Sock s, const Request& r) {
    int i = getIndex(r.query, "file", g_arch.files.size());
    if (i < 0) return fail(s, 400, "Choose a file first.");
    Bytes out; uint64_t fp;
    bool ok = restoreFile(g_arch.files[i], g_state[i].missing, out, fp);
    g_state[i].status = ok ? RESTORED : FAILED;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)fp);
    json(s, string("{\"verified\":") + (ok ? "true" : "false") + ",\"block\":" + to_string(g_state[i].missing) +
            ",\"size\":" + to_string(out.size()) + ",\"fingerprint\":\"" + hex + "\"}");
}

static void apiRestored(Sock s, const Request& r) {
    int i = getIndex(r.query, "file", g_arch.files.size());
    if (i < 0) return fail(s, 400, "Choose a file first.");
    if (g_state[i].status != RESTORED) return fail(s, 409, "Recover this file first.");
    Bytes out; uint64_t fp;
    if (!restoreFile(g_arch.files[i], g_state[i].missing, out, fp)) return fail(s, 500, "Verification failed.");
    respond(s, 200, "application/octet-stream", out);
}

static void apiRestoreAll(Sock s) {
    vector<pair<string, Bytes> > done;
    string failed;
    for (size_t i = 0; i < g_arch.files.size(); i++) {
        Bytes out; uint64_t fp;
        if (restoreFile(g_arch.files[i], g_state[i].missing, out, fp)) {
            g_state[i].status = RESTORED;
            done.push_back(make_pair(g_arch.files[i].name, std::move(out)));
        } else {
            g_state[i].status = FAILED;
            failed += (failed.empty() ? "" : ",") + jsonStr(g_arch.files[i].name);
        }
    }
    g_zip = done.empty() ? Bytes() : makeZip(done);
    json(s, "{\"total\":" + to_string(g_arch.files.size()) + ",\"restored\":" + to_string(done.size()) +
            ",\"failed\":[" + failed + "]}");
}

static void handle(Sock s) {
    Request r;
    if (!readRequest(s, r)) return;
    bool post = r.method == "POST";
    try {
        if (r.path == "/" || r.path == "/index.html") serveFile(s, "web/index.html", "text/html; charset=utf-8");
        else if (r.path == "/style.css") serveFile(s, "web/style.css", "text/css; charset=utf-8");
        else if (r.path == "/script.js") serveFile(s, "web/script.js", "application/javascript; charset=utf-8");
        else if (r.path == "/api/compress" && post) apiCompress(s, r);
        else if (r.path == "/api/open" && post) apiOpen(s, r);
        else if (r.path == "/api/files") json(s, filesJson());
        else if (r.path == "/api/simulate" && post) apiSimulate(s, r);
        else if (r.path == "/api/recover" && post) apiRecover(s, r);
        else if (r.path == "/api/restoreall" && post) apiRestoreAll(s);
        else if (r.path == "/api/restored") apiRestored(s, r);
        else if (r.path == "/api/archive") serveFile(s, ARCHIVE, "application/octet-stream");
        else if (r.path == "/api/zip") {
            if (g_zip.empty()) fail(s, 404, "Nothing restored yet.");
            else respond(s, 200, "application/zip", g_zip);
        } else fail(s, 404, "Not found");
    } catch (const exception& e) {
        fail(s, 500, e.what());
    }
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    Archive last;                                           // reopen the last backup after a restart
    if (loadArchive(ARCHIVE, last)) useArchive(last);
    Sock server = socket(AF_INET, SOCK_STREAM, 0);
#ifndef _WIN32
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
#endif
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          // local machine only
    for (size_t i = 0; i < sizeof addr.sin_zero; i++) addr.sin_zero[i] = 0;
    if (bind(server, (sockaddr*)&addr, sizeof addr) != 0 || listen(server, 16) != 0) {
        cerr << "Could not start server on port " << PORT << " (is it already running?)\n";
        return 1;
    }
    cout << "DataMend running at http://localhost:" << PORT << "  (press Ctrl+C to stop)\n";
    while (true) {
        Sock c = accept(server, 0, 0);
        if (c == BAD_SOCK) continue;
        // Drop idle browser pre-connections instead of blocking on them.
#ifdef _WIN32
        DWORD t = 1500;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
#else
        timeval t; t.tv_sec = 1; t.tv_usec = 500000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
#endif
        handle(c);
        shutdown(c, 1);
        CLOSE_SOCK(c);
    }
}

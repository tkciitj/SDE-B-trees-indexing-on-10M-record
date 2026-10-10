// B+ Tree benchmark: memory-only vs disk-only vs memory+disk (buffered)
#include "bptree.hpp"
#include "stores.hpp"
#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#  include <psapi.h>
#else
#  include <fstream>
#endif

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// ── Peak resident memory of the process ─────────────────────────────────────
static uint64_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof(pmc);
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? (uint64_t)pmc.PeakWorkingSetSize : 0;
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0) { uint64_t kb = 0; std::sscanf(line.c_str(), "VmHWM: %" SCNu64, &kb); return kb * 1024; }
    return 0;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Streaming CSV reader: "key,payload" per line, optional header, LF or CRLF.
// Never silently truncates: a key longer than KEY_LEN is fatal; a payload longer
// than PAYLOAD_LEN is fatal unless --allow-truncate was given.
// ═════════════════════════════════════════════════════════════════════════════
class CSVReader {
    FILE* fp = nullptr;
    std::vector<char> buf;
    size_t pos = 0, len = 0;
    bool eof = false;
    bool first_line = true;

    bool refill() {
        if (eof) return false;
        if (pos > 0) { std::memmove(buf.data(), buf.data() + pos, len - pos); len -= pos; pos = 0; }
        if (len == buf.size()) fatal("CSV line longer than the 4 MB read buffer");
        size_t r = std::fread(buf.data() + len, 1, buf.size() - len, fp);
        if (r == 0) { eof = true; return false; }
        len += r;
        return true;
    }
    // Next raw line (without \n / \r). Pointer valid until the next call.
    bool read_line(const char*& line, size_t& n) {
        while (true) {
            char* nl = (char*)std::memchr(buf.data() + pos, '\n', len - pos);
            if (nl) {
                line = buf.data() + pos; n = (size_t)(nl - (buf.data() + pos));
                pos = (size_t)(nl - buf.data()) + 1;
                if (n && line[n - 1] == '\r') --n;
                return true;
            }
            if (!refill()) {                         // EOF: last line without newline
                if (pos >= len) return false;
                line = buf.data() + pos; n = len - pos; pos = len;
                if (n && line[n - 1] == '\r') --n;
                return true;
            }
        }
    }
public:
    uint64_t lines = 0, truncated_payloads = 0;
    size_t max_key = 0, max_payload = 0, min_key = SIZE_MAX, min_payload = SIZE_MAX;
    bool allow_truncate = false;

    bool open(const char* path) {
        fp = std::fopen(path, "rb");
        if (!fp) return false;
        buf.assign(4u << 20, 0);
        return true;
    }
    void close() { if (fp) { std::fclose(fp); fp = nullptr; } }
    ~CSVReader() { close(); }

    // Returns false at EOF. key/payload are zero padded.
    bool next(char* key, char* payload, bool strict = true) {
        const char* line; size_t n;
        while (read_line(line, n)) {
            if (n == 0) continue;                                   // blank line
            if (first_line) {
                first_line = false;
                if (n >= 4 && (line[0]|32) == 'k' && (line[1]|32) == 'e' && (line[2]|32) == 'y' && line[3] == ',')
                    continue;                                       // header row
            }
            const char* comma = (const char*)std::memchr(line, ',', n);
            if (!comma) fatal("malformed CSV line (no comma)");
            size_t kl = (size_t)(comma - line), pl = n - kl - 1;
            ++lines;
            max_key = std::max(max_key, kl); min_key = std::min(min_key, kl);
            max_payload = std::max(max_payload, pl); min_payload = std::min(min_payload, pl);
            if (!strict) return true;
            if (kl == 0) fatal("empty key in CSV");
            if (kl > KEY_LEN) {
                std::fprintf(stderr, "FATAL: key of %zu chars at record %" PRIu64 " exceeds KEY_LEN=%u.\n"
                             "Rebuild with e.g.  make KEY_LEN=%zu  (run './bpt inspect <csv>' to see max lengths).\n",
                             kl, lines, KEY_LEN, kl);
                std::exit(1);
            }
            if (pl > PAYLOAD_LEN) {
                if (!allow_truncate) {
                    std::fprintf(stderr, "FATAL: payload of %zu bytes at record %" PRIu64 " exceeds PAYLOAD_LEN=%u.\n"
                                 "Rebuild with e.g.  make PAYLOAD_LEN=%zu  or pass --allow-truncate.\n",
                                 pl, lines, PAYLOAD_LEN, pl);
                    std::exit(1);
                }
                ++truncated_payloads; pl = PAYLOAD_LEN;
            }
            std::memset(key, 0, KEY_LEN);         std::memcpy(key, line, kl);
            std::memset(payload, 0, PAYLOAD_LEN); std::memcpy(payload, comma + 1, pl);
            return true;
        }
        return false;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
struct Options {
    std::string mode, csv, out_csv = "results.csv";
    uint64_t limit = UINT64_MAX;
    uint32_t cache_pages = 4096;
    bool cold = false, keep = false, allow_truncate = false;
};

static constexpr uint32_t MAX_SAMPLES = 100000;
static constexpr uint32_t MISS_QUERIES = 10000;
static char sample_keys[MAX_SAMPLES][KEY_LEN];
static char sample_payloads[MAX_SAMPLES][PAYLOAD_LEN];

template <class Store>
static void run_benchmark(Store& store, const Options& o, const char* index_path) {
    BPlusTree<Store> tree(store);
    store.io.reset();                                 // don't count tree initialisation

    CSVReader reader;
    reader.allow_truncate = o.allow_truncate;
    if (!reader.open(o.csv.c_str())) fatal("cannot open dataset");

    const uint64_t interval = (o.limit != UINT64_MAX) ? std::max<uint64_t>(1, o.limit / MAX_SAMPLES) : 100;
    uint32_t num_samples = 0;
    char key[KEY_LEN], payload[PAYLOAD_LEN];
    uint64_t count = 0, duplicates = 0;
    double index_ns = 0;

    // ── INSERT phase ────────────────────────────────────────────────────────
    auto wall0 = Clock::now();
    while (count < o.limit && reader.next(key, payload)) {
        auto t0 = Clock::now();
        bool fresh = tree.insert(key, payload);
        auto t1 = Clock::now();
        index_ns += (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (!fresh) ++duplicates;
        ++count;
        if (count % interval == 0 && num_samples < MAX_SAMPLES) {
            std::memcpy(sample_keys[num_samples], key, KEY_LEN);
            std::memcpy(sample_payloads[num_samples], payload, PAYLOAD_LEN);
            ++num_samples;
        }
        if (count % 1000000 == 0)
            std::printf("  inserted %" PRIu64 "M  (%.1f s wall)\n", count / 1000000, secs(wall0, Clock::now()));
    }
    reader.close();
    auto f0 = Clock::now();
    tree.sync();                                      // header page
    store.sync();                                     // write back all dirty pages + fsync
    double flush_s = secs(f0, Clock::now());
    double wall_s  = secs(wall0, Clock::now());
    double insert_s = index_ns * 1e-9 + flush_s;      // index work only (CSV parsing excluded)
    IOCounters ins_io = store.io;

    std::printf("insert done: %" PRIu64 " records (%" PRIu64 " duplicate keys), index time %.3f s, wall incl. CSV parse %.3f s\n",
                count, duplicates, insert_s, wall_s);

    // ── structural verification ──────────────────────────────────────────────
    uint64_t verified = 0;
    bool structure_ok = tree.verify(&verified);       // full walk: ordering, ranges, level uniformity, leaf chain
    store.io.reset();

    // ── SEARCH phase ────────────────────────────────────────────────────────
    if (o.cold) store.drop_caches();
    store.io.reset();
    std::vector<uint32_t> lat_ns; lat_ns.reserve(num_samples + MISS_QUERIES);
    char out[PAYLOAD_LEN];
    uint32_t hit = 0, bad_payload = 0, false_pos = 0, miss_q = std::min<uint32_t>(MISS_QUERIES, num_samples);
    auto s0 = Clock::now();
    for (uint32_t i = 0; i < num_samples; ++i) {
        auto a = Clock::now();
        bool f = tree.search(sample_keys[i], out);
        auto b = Clock::now();
        lat_ns.push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (f) { ++hit; if (std::memcmp(out, sample_payloads[i], PAYLOAD_LEN) != 0) ++bad_payload; }
    }
    for (uint32_t i = 0; i < miss_q; ++i) {           // keys that cannot exist: contain byte 0x01
        char bad[KEY_LEN]; std::memcpy(bad, sample_keys[i], KEY_LEN);
        size_t kl = strnlen(bad, KEY_LEN);
        bad[kl ? kl - 1 : 0] = '\x01';
        auto a = Clock::now();
        bool f = tree.search(bad, nullptr);
        auto b = Clock::now();
        lat_ns.push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (f) ++false_pos;
    }
    double search_s = secs(s0, Clock::now());
    IOCounters srch_io = store.io;
    uint32_t total_q = num_samples + miss_q;
    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) { return lat_ns.empty() ? 0.0 : lat_ns[std::min<size_t>(lat_ns.size() - 1, (size_t)(p * lat_ns.size()))] / 1000.0; };

    // ── numbers for the report ─────────────────────────────────────────────
    uint64_t rss = peak_rss_bytes();
    uint64_t disk_bytes = store.file_size();
    uint64_t index_bytes = (uint64_t)tree.total_pages() * PAGE_SIZE;
    double fill = tree.leaf_nodes() ? (double)tree.record_count() / ((double)tree.leaf_nodes() * MAX_LRECS) : 0;
    const double MB = 1024.0 * 1024.0;
    const bool is_buf = (o.mode == "buf");

    std::printf("\n══════════ RESULTS  mode=%s  N=%" PRIu64 "  cache_pages=%u%s ══════════\n",
                o.mode.c_str(), count, is_buf ? o.cache_pages : 0, o.cold ? "  (cold search)" : "");
    std::printf("Insertion (index only): %.3f s   avg %.2f us/insert   [wall incl. CSV parsing: %.3f s]\n",
                insert_s, insert_s / (double)count * 1e6, wall_s);
    std::printf("Search: %u queries (%u hits, %u misses) in %.4f s\n", total_q, num_samples, miss_q, search_s);
    std::printf("        avg %.2f us   p50 %.2f us   p99 %.2f us\n", search_s / total_q * 1e6, pct(0.5), pct(0.99));
    std::printf("Correctness: found %u/%u existing, payload mismatches %u, false positives %u/%u%s\n",
                hit, num_samples, bad_payload, false_pos, miss_q,
                (hit != num_samples || bad_payload || false_pos) ? "   *** ERROR ***" : "");
    std::printf("Structure check: %s (%" PRIu64 " records in leaf chain)\n", structure_ok ? "OK" : "*** FAILED ***", verified);
    std::printf("Tree: height %u, %u leaves, %u internal, %" PRIu64 " records, avg leaf fill %.1f%%\n",
                tree.height(), tree.leaf_nodes(), tree.internal_nodes(), tree.record_count(), fill * 100);
    std::printf("Index size (logical): %.1f MB (%u pages)\n", index_bytes / MB, tree.total_pages());
    std::printf("Memory: peak RSS %.1f MB   (store-resident data structures: %.1f MB)\n", rss / MB, store.mem_bytes() / MB);
    std::printf("Disk usage: %.1f MB (%" PRIu64 " bytes)\n", disk_bytes / MB, disk_bytes);
    std::printf("Insert I/O: page reads %" PRIu64 "  page writes %" PRIu64 "  (%.1f MB read, %.1f MB written)\n",
                ins_io.page_reads, ins_io.page_writes, ins_io.bytes_read() / MB, ins_io.bytes_written() / MB);
    std::printf("            per insert: %.3f reads, %.3f writes\n",
                (double)ins_io.page_reads / count, (double)ins_io.page_writes / count);
    if (is_buf) std::printf("            buffer hits %" PRIu64 "  misses %" PRIu64 "  hit ratio %.4f\n",
                            ins_io.buf_hits, ins_io.buf_misses, ins_io.hit_ratio());
    std::printf("Search I/O: page reads %" PRIu64 "  page writes %" PRIu64 "  (%.3f reads/query)\n",
                srch_io.page_reads, srch_io.page_writes, (double)srch_io.page_reads / total_q);
    if (is_buf) std::printf("            buffer hits %" PRIu64 "  misses %" PRIu64 "  hit ratio %.4f\n",
                            srch_io.buf_hits, srch_io.buf_misses, srch_io.hit_ratio());
    if (reader.truncated_payloads) std::printf("WARNING: %" PRIu64 " payloads were truncated to %u bytes\n", reader.truncated_payloads, PAYLOAD_LEN);

    // ── append one row to the results CSV ───────────────────────────────────
    bool need_header = false;
    if (FILE* t = std::fopen(o.out_csv.c_str(), "rb")) std::fclose(t); else need_header = true;
    if (FILE* f = std::fopen(o.out_csv.c_str(), "ab")) {
        if (need_header)
            std::fprintf(f, "mode,N,cache_pages,page_size,key_len,payload_len,insert_s,avg_insert_us,wall_s,avg_search_us,p50_search_us,p99_search_us,"
                            "height,leaves,internals,leaf_fill_pct,index_mb,peak_rss_mb,disk_mb,"
                            "ins_reads,ins_writes,ins_hits,ins_misses,srch_reads,srch_writes,srch_hits,srch_misses,cold\n");
        std::fprintf(f, "%s,%" PRIu64 ",%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%.2f,%.1f,%.1f,%.1f,"
                        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d\n",
                     o.mode.c_str(), count, is_buf ? o.cache_pages : 0, PAGE_SIZE, KEY_LEN, PAYLOAD_LEN,
                     insert_s, insert_s / (double)count * 1e6, wall_s, search_s / total_q * 1e6, pct(0.5), pct(0.99),
                     tree.height(), tree.leaf_nodes(), tree.internal_nodes(), fill * 100, index_bytes / MB, rss / MB, disk_bytes / MB,
                     ins_io.page_reads, ins_io.page_writes, ins_io.buf_hits, ins_io.buf_misses,
                     srch_io.page_reads, srch_io.page_writes, srch_io.buf_hits, srch_io.buf_misses, o.cold ? 1 : 0);
        std::fclose(f);
        std::printf("Row appended to %s\n", o.out_csv.c_str());
    }
    (void)index_path;
    if (hit != num_samples || bad_payload || false_pos || !structure_ok) std::exit(2);
}

// ═════════════════════════════════════════════════════════════════════════════
// inspect: scan a CSV and report key/payload lengths (to choose KEY_LEN / PAYLOAD_LEN)
// ═════════════════════════════════════════════════════════════════════════════
static int run_inspect(const char* path) {
    CSVReader r;
    if (!r.open(path)) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    char k[KEY_LEN], p[PAYLOAD_LEN];
    while (r.next(k, p, /*strict=*/false)) {}
    std::printf("records: %" PRIu64 "\nkey length:     min %zu  max %zu   (built with KEY_LEN=%u)\n"
                "payload length: min %zu  max %zu   (built with PAYLOAD_LEN=%u)\n",
                r.lines, r.min_key, r.max_key, KEY_LEN, r.min_payload, r.max_payload, PAYLOAD_LEN);
    if (r.max_key > KEY_LEN)         std::printf("-> rebuild with KEY_LEN=%zu\n", r.max_key);
    if (r.max_payload > PAYLOAD_LEN) std::printf("-> rebuild with PAYLOAD_LEN=%zu\n", r.max_payload);
    if (r.max_key <= KEY_LEN && r.max_payload <= PAYLOAD_LEN) std::printf("-> current build fits this dataset\n");
    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Self-test: compares the tree against std::map (test code only) on all stores
// ═════════════════════════════════════════════════════════════════════════════
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  *** FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)

template <class Store>
static void test_one(Store& store, const char* name, uint32_t n, uint32_t seed, int pattern) {
    BPlusTree<Store> tree(store);
    std::mt19937_64 rng(seed);
    static const char AL[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::map<std::string, std::string> ref;

    auto mk = [&](uint32_t i, std::string& k, std::string& p) {
        k.assign(KEY_LEN, '0');
        if (pattern == 0) for (uint32_t j = 0; j < KEY_LEN; ++j) k[j] = AL[rng() % 62];          // random
        else {                                                                                   // ascending / descending
            uint32_t v = (pattern == 1) ? i : (0xFFFFFFu - i);
            for (int j = (int)KEY_LEN - 1; j >= 0 && v; --j) { k[j] = AL[v % 62]; v /= 62; }
        }
        p.assign(PAYLOAD_LEN, 0);
        for (uint32_t j = 0; j < PAYLOAD_LEN; ++j) p[j] = AL[rng() % 62];
    };

    std::vector<std::string> keys;
    for (uint32_t i = 0; i < n; ++i) {
        std::string k, p; mk(i, k, p);
        bool fresh = tree.insert(k.data(), p.data());
        bool ref_fresh = ref.find(k) == ref.end();
        CHECK(fresh == ref_fresh, "insert() freshness flag");
        ref[k] = p;
        keys.push_back(k);
    }
    // overwrite 10% existing keys with new payloads
    for (uint32_t i = 0; i < n / 10; ++i) {
        const std::string& k = keys[rng() % keys.size()];
        std::string p(PAYLOAD_LEN, 'x'); p[0] = (char)('A' + i % 26);
        CHECK(!tree.insert(k.data(), p.data()), "overwrite must report not-new");
        ref[k] = p;
    }
    CHECK(tree.record_count() == ref.size(), "record_count == distinct keys");
    uint64_t chain = 0;
    CHECK(tree.verify(&chain), "structure verification");
    CHECK(chain == ref.size(), "leaf chain count");

    char out[PAYLOAD_LEN];
    for (auto& kv : ref) {                                          // every key found with latest payload
        CHECK(tree.search(kv.first.data(), out), "existing key found");
        CHECK(std::memcmp(out, kv.second.data(), PAYLOAD_LEN) == 0, "payload matches");
    }
    for (uint32_t i = 0; i < 2000; ++i) {                           // absent keys
        std::string k(KEY_LEN, 'a'); for (auto& c : k) c = AL[rng() % 62];
        k[0] = '\x01';
        CHECK(!tree.search(k.data(), out), "absent key not found");
    }
    for (uint32_t i = 0; i < 200; ++i) {                            // range counts vs std::map
        std::string a(KEY_LEN, 0), b(KEY_LEN, 0);
        for (auto& c : a) c = AL[rng() % 62];
        for (auto& c : b) c = AL[rng() % 62];
        if (a > b) std::swap(a, b);
        uint64_t expect = std::distance(ref.lower_bound(a), ref.lower_bound(b));
        CHECK(tree.range_count(a.data(), b.data()) == expect, "range_count");
    }
    store.drop_caches();                                            // data must survive eviction / reload
    for (auto& kv : ref) {
        CHECK(tree.search(kv.first.data(), out) && std::memcmp(out, kv.second.data(), PAYLOAD_LEN) == 0, "after cache drop");
    }
    std::printf("  [%-13s] pattern=%d n=%-7u distinct=%-7zu height=%u leaves=%u internals=%u  OK\n",
                name, pattern, n, ref.size(), tree.height(), tree.leaf_nodes(), tree.internal_nodes());
}

static void run_test() {
    std::printf("Self-test (PAGE_SIZE=%u KEY_LEN=%u PAYLOAD_LEN=%u, leaf cap %u, internal cap %u keys)\n",
                PAGE_SIZE, KEY_LEN, PAYLOAD_LEN, MAX_LRECS, MAX_IKEYS);
    const uint32_t big = (MAX_LRECS < 10) ? 60000 : 200000;
    for (int pattern = 0; pattern < 3; ++pattern) {
        { MemStore s;                               test_one(s, "MemStore", big, 1 + pattern, pattern); }
        { DiskStore s("test_disk.db");              test_one(s, "DiskStore", big / 4, 2 + pattern, pattern); }
        { BufferedStore s("test_buf_small.db", 4);  test_one(s, "Buffered(4)", big / 2, 3 + pattern, pattern); }
        { BufferedStore s("test_buf.db", 256);      test_one(s, "Buffered(256)", big, 4 + pattern, pattern); }
    }
    for (uint32_t n : {1u, 2u, MAX_LRECS, MAX_LRECS + 1, 3 * MAX_LRECS}) {   // boundary sizes
        MemStore s; test_one(s, "MemStore", n, 99, 0);
    }
    std::remove("test_disk.db"); std::remove("test_buf_small.db"); std::remove("test_buf.db");
    std::printf("All tests PASSED.\n");
}

// ═════════════════════════════════════════════════════════════════════════════
static void usage(const char* a) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s test\n"
        "  %s inspect <csv>\n"
        "  %s <mem|disk|buf> <csv> [--limit N] [--cache-pages N] [--cold] [--out results.csv] [--keep] [--allow-truncate]\n", a, a, a);
}

int main(int argc, char** argv) {
    std::printf("B+ Tree  PAGE_SIZE=%u  KEY_LEN=%u  PAYLOAD_LEN=%u  leaf capacity=%u records  internal capacity=%u keys (%u children)\n",
                PAGE_SIZE, KEY_LEN, PAYLOAD_LEN, MAX_LRECS, MAX_IKEYS, MAX_IKEYS + 1);
    if (argc < 2) { usage(argv[0]); return 1; }
    Options o; o.mode = argv[1];
    if (o.mode == "test") { run_test(); return 0; }
    if (argc < 3) { usage(argv[0]); return 1; }
    if (o.mode == "inspect") return run_inspect(argv[2]);
    if (o.mode != "mem" && o.mode != "disk" && o.mode != "buf") { usage(argv[0]); return 1; }
    o.csv = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(1); } return argv[++i]; };
        if (a == "--limit") o.limit = std::strtoull(need("--limit"), nullptr, 10);
        else if (a == "--cache-pages") o.cache_pages = (uint32_t)std::strtoul(need("--cache-pages"), nullptr, 10);
        else if (a == "--out") o.out_csv = need("--out");
        else if (a == "--cold") o.cold = true;
        else if (a == "--keep") o.keep = true;
        else if (a == "--allow-truncate") o.allow_truncate = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(argv[0]); return 1; }
    }
    std::printf("mode=%s dataset=%s", o.mode.c_str(), o.csv.c_str());
    if (o.limit != UINT64_MAX) std::printf(" limit=%" PRIu64, o.limit);
    if (o.mode == "buf") std::printf(" cache=%u pages (%.1f MB)", o.cache_pages, o.cache_pages * (double)PAGE_SIZE / (1024.0 * 1024.0));
    std::printf("\n");

    if (o.mode == "mem") {
        MemStore s; run_benchmark(s, o, "");
    } else if (o.mode == "disk") {
        const char* p = "index_disk.db";
        { DiskStore s(p); run_benchmark(s, o, p); }
        if (!o.keep) std::remove(p);
    } else {
        const char* p = "index_buf.db";
        { BufferedStore s(p, o.cache_pages); run_benchmark(s, o, p); }
        if (!o.keep) std::remove(p);
    }
    return 0;
}

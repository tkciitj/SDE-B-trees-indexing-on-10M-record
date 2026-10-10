// Test-data generator (only needed if you don't have the assignment CSV).
// Usage: gen_dataset <n> <out.csv> [key_len=8] [payload_len=64] [seed=1]
// Produces "key,payload" with unique random alphanumeric keys in random order.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <n> <out.csv> [key_len] [payload_len] [seed]\n", argv[0]); return 1; }
    uint64_t n = std::strtoull(argv[1], nullptr, 10);
    int kl = argc > 3 ? std::atoi(argv[3]) : 8, pl = argc > 4 ? std::atoi(argv[4]) : 64;
    uint64_t seed = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 1;
    if (kl > 10) { std::fprintf(stderr, "key_len <= 10 supported\n"); return 1; }
    static const char AL[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint64_t space = 1; for (int i = 0; i < kl; ++i) space *= 62;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> ids;
    while (ids.size() < n) {                       // draw, sort, dedupe until n unique ids
        size_t need = n - ids.size();
        for (size_t i = 0; i < need + need / 50 + 16; ++i) ids.push_back(rng() % space);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    ids.resize(n);
    std::shuffle(ids.begin(), ids.end(), rng);     // random order
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 1;
    std::fprintf(f, "key,payload\n");
    std::vector<char> line(kl + pl + 3);
    for (uint64_t id : ids) {
        for (int j = kl - 1; j >= 0; --j) { line[j] = AL[id % 62]; id /= 62; }
        line[kl] = ',';
        for (int j = 0; j < pl; ++j) line[kl + 1 + j] = AL[rng() % 62];
        line[kl + 1 + pl] = '\n';
        std::fwrite(line.data(), 1, kl + pl + 2, f);
    }
    std::fclose(f);
    return 0;
}

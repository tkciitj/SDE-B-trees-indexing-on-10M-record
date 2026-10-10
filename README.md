# B+ Tree Index — Memory-only, Disk-only, Memory+Disk (buffered)

A B+ tree written from scratch in C++17 (no index/DB library, STL only for plumbing such as `std::vector`;
`std::map` is used **only inside the self-test** as a reference oracle).

## Build / run

```bash
make                         # builds ./bpt   (Windows/MinGW: bpt.exe, links -lpsapi)
make test                    # self-tests: 4 KB pages + 256-byte-page stress build with ASan/UBSan
./bpt inspect data.csv       # prints max key/payload length -> tells you if KEY_LEN/PAYLOAD_LEN fit
./run_all.sh data.csv        # build + test + every benchmark configuration -> results.csv
```

```
./bpt <mem|disk|buf> <csv> [--limit N] [--cache-pages N] [--cold] [--out results.csv] [--keep] [--allow-truncate]
```

| option | meaning |
|---|---|
| `--limit N` | only insert the first N rows |
| `--cache-pages N` | buffer-pool size for `buf` mode (default 4096 pages = 16 MB) |
| `--cold` | before the search phase: flush, drop the buffer pool and (Linux) the OS page cache, so searches really hit the device |
| `--keep` | keep `index_*.db` after the run |
| `--allow-truncate` | allow payloads longer than `PAYLOAD_LEN` (default: abort) |

**If your CSV has keys longer than 8 chars or payloads longer than 64 bytes** the program stops with a clear
message (it never truncates silently). Rebuild with e.g. `make KEY_LEN=12 PAYLOAD_LEN=100`.

## Assumptions

| Item | Value |
|---|---|
| Page / node size | 4096 bytes (one OS/disk page); node = page |
| Key | fixed 8 bytes, zero-padded, compared with `memcmp` (alphanumeric, unique) |
| Payload | fixed 64 bytes, zero-padded |
| Node header | 8 B (`is_leaf`, `num_keys`, `next_leaf`) |
| **Internal node** | 340 keys / **341 children** (4 B child page numbers) → order m = 341 |
| **Leaf node** | 56 records of 72 B; leaves linked left→right (`next_leaf`) for range scans |
| Split policy | leaf: 57 records → 28 + 29, first key of right leaf is *copied* up; internal: middle key *moved* up |
| Duplicates | an existing key is overwritten (dataset keys are unique, so this never triggers) |
| Deletes | not required, not implemented |
| Insert path | top-down recursive insert, splits propagate upward, new root on root split |
| Metadata | page 0 (root page, height, record/leaf/internal counts, key/payload/page sizes) |

### File layout (disk / buffered)
`page 0` = metadata, `page 1…` = tree nodes allocated sequentially; byte offset = `page_no × 4096`.

## Three storage back-ends (same tree code, templated on the store)

| Mode | Where pages live | Cache | Writes |
|---|---|---|---|
| `mem`  | RAM arena (64 MB chunks) | – (everything in RAM) | none |
| `disk` | file only | **none** (2 scratch pages; every page access = 1 `pread`) | write-through (every modified page = 1 `pwrite`) |
| `buf`  | file + RAM | LRU buffer pool, N pages, hash-table lookup | write-back (on eviction / flush) |

The tree keeps only the 40-byte header in RAM (persisted to page 0 when the root changes and at the end).
Tree code never holds a `Page*` across another store call, so frames can be recycled safely.

## What is measured
Insertion time (index work only, CSV parsing excluded, plus final flush; wall time incl. parsing also printed),
search time (100 000 existing keys + 10 000 absent keys; avg / p50 / p99), peak RSS, logical index size,
disk usage, tree height, leaf/internal counts, leaf fill, page reads/writes (and MB), buffer hits/misses/hit ratio.
Every run also verifies: all sampled keys found with the right payload, absent keys not found, and a **full structural
check** (ordering inside every node, separator ranges, all leaves at one depth, sorted leaf chain, counts).

## Files
`src/bptree.hpp` tree · `src/stores.hpp` stores · `src/main.cpp` CLI/benchmark/tests ·
`tools/gen_dataset.cpp` optional test-data generator · `REPORT.md` results · `results_synthetic_10m.csv` raw numbers.

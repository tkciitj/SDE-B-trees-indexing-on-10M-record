# B+ Tree on 10,000,000 records — results & analysis

**Setup of these numbers:** Linux sandbox, 1 vCPU, 3 GB RAM, ext4/virtio disk, g++ 13 `-O2`.
Data: 10 M unique random 8-char alphanumeric keys, 64-char payloads, shuffled (synthetic, same shape as the
assignment). Re-run `./run_all.sh <your csv>` on your machine and paste your own numbers — absolute times will differ.

## Design assumptions
Page = node = 4096 B · key 8 B · payload 64 B · internal order 341 (340 keys) · leaf 56 records ·
leaf split 28/29 · linked leaves · page 0 = metadata. Disk-only = no cache, write-through.
Memory+disk = LRU write-back buffer pool (size varied below).

## Results (N = 10,000,000; search = 100 k hits + 10 k misses, cold caches)

| Metric | Memory-only | Disk-only | Mem+Disk 16 MB pool | Mem+Disk 64 MB pool | Mem+Disk 256 MB pool |
|---|---|---|---|---|---|
| Insert total (s) | 10.7 | 38.2 | 38.1 | 38.4 | 28.2 |
| Insert avg (µs) | 1.07 | 3.82 | 3.81 | 3.84 | 2.82 |
| Search avg (µs) | 1.27 | 24.9 | 23.6 | 24.3 | 24.1 |
| Search p99 (µs) | 1.8 | 61.7 | 58.8 | 69.1 | 68.9 |
| Tree height | 4 | 4 | 4 | 4 | 4 |
| Leaves / internal nodes | 256,419 / 1,029 | same | same | same | same |
| Avg leaf fill | 69.6 % | 69.6 % | 69.6 % | 69.6 % | 69.6 % |
| Peak RSS (MB) | 1020.7 | 15.1 | 31.2 | 79.6 | 273.1 |
| Disk usage (MB) | 0 | 1005.7 | 1005.7 | 1005.7 | 1005.7 |
| Insert page reads (M) | 0 | 36.87 | 9.36 | 7.59 | 3.90 |
| Insert page writes (M) | 0 | 10.51 | 9.52 | 7.85 | 4.16 |
| Reads / insert | 0 | 3.69 | 0.94 | 0.76 | 0.39 |
| Writes / insert | 0 | 1.05 | 0.95 | 0.79 | 0.42 |
| Buffer hit ratio (insert) | – | – | 75.0 % | 79.7 % | 89.6 % |
| Search reads / query | 0 | 4.00 | 1.04 | 0.95 | 0.84 |
| Buffer hit ratio (search) | – | – | 73.9 % | 76.2 % | 79.1 % |

All runs: 100,000/100,000 existing keys found with correct payload, 0/10,000 false positives, full structure check OK.

## Analysis
* **Height 4** for 10 M keys: 341-way fan-out, but random inserts leave leaves ~70 % full, so ≈256 k leaves → 1,029 internal nodes → root. Internal levels are only 0.4 % of the index (≈4 MB), which is why even a small pool caches them.
* **Memory-only** is fastest (1.07 µs insert, 1.3 µs search) but needs ~1 GB RAM (index ≈ 1006 MB; the CSV's 740 MB raw size is smaller because pages are 70 % full and carry node overhead).
* **Disk-only**: every level of every operation is a page read: exactly 4.0 reads/query (= height), 3.69 reads + 1.05 writes per insert (the extra write is the split/ metadata traffic). Cold search is ~20× slower than memory.
* **Buffered**: a 16 MB pool (1.6 % of the index) already cuts insert reads by 4× and search reads to ~1/query, because the internal levels stay cached and only the leaf is fetched. Larger pools reduce I/O further, but gains are bounded because inserts are *random* (leaf hit chance ≈ pool/leaf count).
* **Caveat on times:** Insert time for disk-only and the 16/64 MB pools is nearly identical here because the sandbox's OS page cache absorbs the `pread`/`pwrite` calls (the cost is dominated by syscalls, not device latency). The I/O *counts* are the reliable comparison; on a machine with a slower disk, or with direct I/O, the gap between disk-only and buffered widens. `--cold` drops the OS cache before searching, which is why search shows real device latency (~25 µs).
* **Trade-off summary:** memory-only = speed, 1 GB RAM, no persistence · disk-only = minimal RAM (15 MB), persistent, slowest · memory+disk = tunable RAM/I/O trade-off, persistent, near-memory search for hot pages.

## Correctness testing
`make test`: random, ascending and descending inserts against a `std::map` oracle on all three stores (incl. a 4-page pool to force constant eviction), duplicate overwrite, absent keys, range counts, reload after cache drop, boundary sizes; repeated with 256-byte pages (3 records/leaf, 20 keys/internal, height 5) under AddressSanitizer + UBSan.

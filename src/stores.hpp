#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Page stores used by the B+ tree.
//   MemStore      – all pages in RAM (chunked arena, no disk)
//   DiskStore     – no cache: 2 scratch pages, write-through, every miss = pread
//   BufferedStore – LRU buffer pool (write-back) in RAM + page file on disk
// ─────────────────────────────────────────────────────────────────────────────
#include "bptree.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#endif

[[noreturn]] static inline void fatal(const char* msg) {
    std::fprintf(stderr, "FATAL: %s\n", msg);
    std::exit(1);
}

// ── I/O counters ────────────────────────────────────────────────────────────
struct IOCounters {
    uint64_t page_reads  = 0;   // pages read from the file
    uint64_t page_writes = 0;   // pages written to the file
    uint64_t buf_hits    = 0;   // buffered mode only
    uint64_t buf_misses  = 0;
    void reset() { *this = IOCounters(); }
    uint64_t bytes_read()    const { return page_reads  * (uint64_t)PAGE_SIZE; }
    uint64_t bytes_written() const { return page_writes * (uint64_t)PAGE_SIZE; }
    double   hit_ratio()     const {
        uint64_t t = buf_hits + buf_misses;
        return t ? (double)buf_hits / (double)t : 0.0;
    }
};

// ── Thin portable wrapper around a page file (pread/pwrite or ReadFile/WriteFile)
class PageFile {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
public:
    IOCounters io;

    PageFile() = default;
    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    ~PageFile() { close_file(); }

    void open_new(const char* path) {      // create / truncate
#ifdef _WIN32
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (h == INVALID_HANDLE_VALUE) fatal("cannot create index file");
#else
        fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) fatal("cannot create index file");
#endif
    }

    void read_page(uint32_t pno, void* buf) {
        io.page_reads++;
        const uint64_t off = (uint64_t)pno * PAGE_SIZE;
        size_t got = 0;
#ifdef _WIN32
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD n = 0;
        if (!ReadFile(h, buf, PAGE_SIZE, &n, &ov) && GetLastError() != ERROR_HANDLE_EOF)
            fatal("ReadFile failed");
        got = n;
#else
        while (got < PAGE_SIZE) {
            ssize_t r = ::pread(fd, (char*)buf + got, PAGE_SIZE - got, (off_t)(off + got));
            if (r < 0) fatal("pread failed");
            if (r == 0) break;                       // beyond EOF
            got += (size_t)r;
        }
#endif
        if (got < PAGE_SIZE) std::memset((char*)buf + got, 0, PAGE_SIZE - got);
    }

    void write_page(uint32_t pno, const void* buf) {
        io.page_writes++;
        const uint64_t off = (uint64_t)pno * PAGE_SIZE;
#ifdef _WIN32
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD n = 0;
        if (!WriteFile(h, buf, PAGE_SIZE, &n, &ov) || n != PAGE_SIZE) fatal("WriteFile failed");
#else
        size_t done = 0;
        while (done < PAGE_SIZE) {
            ssize_t w = ::pwrite(fd, (const char*)buf + done, PAGE_SIZE - done, (off_t)(off + done));
            if (w < 0) fatal("pwrite failed");
            done += (size_t)w;
        }
#endif
    }

    void sync() {                                    // force data to the device
#ifdef _WIN32
        FlushFileBuffers(h);
#else
        ::fsync(fd);
#endif
    }

    // Ask the OS to forget cached copies of the file (Linux only; no-op elsewhere),
    // so a following search phase really hits the device ("cold" run).
    void drop_os_cache() {
#if defined(__linux__)
        ::fsync(fd);
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    }

    void close_file() {
#ifdef _WIN32
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
#else
        if (fd >= 0) { ::close(fd); fd = -1; }
#endif
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// 1. MemStore – chunked arena (64 MB chunks), pointers never move, no disk
// ═════════════════════════════════════════════════════════════════════════════
class MemStore {
    static constexpr uint32_t CHUNK_PAGES = (64u * 1024 * 1024) / PAGE_SIZE;
    std::vector<char*> chunks;
    uint32_t num_pages = 0;
public:
    IOCounters io;                                   // always zero for this store

    MemStore() = default;
    MemStore(const MemStore&) = delete;
    MemStore& operator=(const MemStore&) = delete;
    ~MemStore() { for (char* c : chunks) std::free(c); }

    Page* get(uint32_t pno) {
        return reinterpret_cast<Page*>(chunks[pno / CHUNK_PAGES] + (uint64_t)(pno % CHUNK_PAGES) * PAGE_SIZE);
    }
    void mark_dirty(uint32_t) {}
    uint32_t alloc() {
        if (num_pages / CHUNK_PAGES >= chunks.size()) {
            char* c = (char*)std::calloc(CHUNK_PAGES, PAGE_SIZE);   // zeroed
            if (!c) fatal("MemStore: out of memory");
            chunks.push_back(c);
        }
        return num_pages++;
    }
    void flush() {}
    void sync() {}
    void drop_caches() {}
    uint64_t file_size() const { return 0; }                         // no disk usage
    uint64_t mem_bytes() const { return (uint64_t)num_pages * PAGE_SIZE; }
};

// ═════════════════════════════════════════════════════════════════════════════
// 2. DiskStore – disk-only. No page cache of our own: just 2 scratch pages so a
//    caller can look at one page at a time (+ the page it just touched).
//    Every get() of a page that is not in scratch → one pread().
//    mark_dirty() is write-through → one pwrite().
//    (The OS file cache can still help; use --cold to drop it before searching.)
// ═════════════════════════════════════════════════════════════════════════════
class DiskStore {
    static constexpr uint32_t SCRATCH = 2;
    PageFile file;
    alignas(8) char scratch[SCRATCH][PAGE_SIZE];
    uint32_t map[SCRATCH];
    bool     dirty[SCRATCH];
    uint32_t next_victim = 0;
    uint32_t total_pages = 0;

    uint32_t take_slot() {                           // free slot first, else round-robin
        for (uint32_t i = 0; i < SCRATCH; ++i)
            if (map[i] == INVALID_PAGE) return i;
        uint32_t s = next_victim;
        next_victim = (next_victim + 1) % SCRATCH;
        flush_slot(s);
        return s;
    }
    void flush_slot(uint32_t s) {
        if (map[s] != INVALID_PAGE && dirty[s]) { file.write_page(map[s], scratch[s]); dirty[s] = false; }
    }
public:
    IOCounters& io;

    explicit DiskStore(const char* path) : io(file.io) {
        for (uint32_t i = 0; i < SCRATCH; ++i) { map[i] = INVALID_PAGE; dirty[i] = false; }
        file.open_new(path);
    }
    ~DiskStore() { flush(); }

    Page* get(uint32_t pno) {
        for (uint32_t i = 0; i < SCRATCH; ++i)
            if (map[i] == pno) return reinterpret_cast<Page*>(scratch[i]);
        uint32_t s = take_slot();
        file.read_page(pno, scratch[s]);
        map[s] = pno; dirty[s] = false;
        return reinterpret_cast<Page*>(scratch[s]);
    }
    void mark_dirty(uint32_t pno) {
        for (uint32_t i = 0; i < SCRATCH; ++i)
            if (map[i] == pno) { file.write_page(pno, scratch[i]); dirty[i] = false; return; }
        fatal("DiskStore::mark_dirty on a page that is not in scratch");
    }
    uint32_t alloc() {                               // new zero page, no read needed
        uint32_t pno = total_pages++;
        uint32_t s = take_slot();
        std::memset(scratch[s], 0, PAGE_SIZE);
        map[s] = pno; dirty[s] = true;               // guaranteed to reach disk
        return pno;
    }
    void flush() { for (uint32_t i = 0; i < SCRATCH; ++i) flush_slot(i); }
    void sync() { flush(); file.sync(); }
    void drop_caches() { flush(); for (uint32_t i = 0; i < SCRATCH; ++i) map[i] = INVALID_PAGE; file.drop_os_cache(); }
    uint64_t file_size() const { return (uint64_t)total_pages * PAGE_SIZE; }
    uint64_t mem_bytes() const { return sizeof(scratch); }
};

// ═════════════════════════════════════════════════════════════════════════════
// 3. BufferedStore – memory + disk. Fixed-size LRU buffer pool, write-back:
//    dirty pages are written only when evicted or flushed.
// ═════════════════════════════════════════════════════════════════════════════
class BufferedStore {
    static constexpr uint32_t NIL = 0xFFFFFFFFu;

    struct Frame {
        char     data[PAGE_SIZE];
        uint32_t page_no;
        uint32_t lru_prev, lru_next;
        uint8_t  dirty, valid;
    };
    struct HTEntry { uint32_t page_no, frame_idx; };

    PageFile file;
    uint32_t total_pages = 0;
    Frame*   frames = nullptr;
    uint32_t pool_cap;
    uint32_t used_frames = 0;                        // frames handed out so far
    uint32_t lru_head = NIL, lru_tail = NIL;         // head = MRU, tail = LRU
    HTEntry* ht = nullptr;                           // open addressing, linear probing
    uint32_t ht_cap = 1, ht_shift = 0;

    uint32_t ht_slot(uint32_t pno) const { return (uint32_t)(pno * 2654435769u) >> ht_shift; }

    int64_t ht_find(uint32_t pno) const {
        uint32_t i = ht_slot(pno);
        while (true) {
            if (ht[i].page_no == pno) return ht[i].frame_idx;
            if (ht[i].page_no == NIL) return -1;
            i = (i + 1) & (ht_cap - 1);
        }
    }
    void ht_insert(uint32_t pno, uint32_t fi) {
        uint32_t i = ht_slot(pno);
        while (ht[i].page_no != NIL) i = (i + 1) & (ht_cap - 1);
        ht[i].page_no = pno; ht[i].frame_idx = fi;
    }
    void ht_remove(uint32_t pno) {                   // delete + re-insert the rest of the cluster
        uint32_t i = ht_slot(pno);
        while (ht[i].page_no != pno) { if (ht[i].page_no == NIL) return; i = (i + 1) & (ht_cap - 1); }
        ht[i].page_no = NIL;
        uint32_t j = (i + 1) & (ht_cap - 1);
        while (ht[j].page_no != NIL) {
            HTEntry e = ht[j];
            ht[j].page_no = NIL;
            ht_insert(e.page_no, e.frame_idx);
            j = (j + 1) & (ht_cap - 1);
        }
    }

    void lru_remove(uint32_t fi) {
        uint32_t p = frames[fi].lru_prev, n = frames[fi].lru_next;
        if (p != NIL) frames[p].lru_next = n; else lru_head = n;
        if (n != NIL) frames[n].lru_prev = p; else lru_tail = p;
        frames[fi].lru_prev = frames[fi].lru_next = NIL;
    }
    void lru_push_front(uint32_t fi) {
        frames[fi].lru_prev = NIL;
        frames[fi].lru_next = lru_head;
        if (lru_head != NIL) frames[lru_head].lru_prev = fi;
        lru_head = fi;
        if (lru_tail == NIL) lru_tail = fi;
    }

    uint32_t grab_frame() {                          // unused frame, else evict LRU
        if (used_frames < pool_cap) return used_frames++;
        uint32_t fi = lru_tail;
        lru_remove(fi);
        if (frames[fi].dirty) { file.write_page(frames[fi].page_no, frames[fi].data); frames[fi].dirty = 0; }
        ht_remove(frames[fi].page_no);
        frames[fi].valid = 0;
        return fi;
    }

public:
    IOCounters& io;

    BufferedStore(const char* path, uint32_t cache_pages) : pool_cap(cache_pages), io(file.io) {
        if (cache_pages < 4) fatal("BufferedStore: cache must be at least 4 pages");
        frames = (Frame*)std::calloc(pool_cap, sizeof(Frame));
        if (!frames) fatal("BufferedStore: out of memory (frames)");
        while (ht_cap < pool_cap * 2) ht_cap <<= 1;
        uint32_t bits = 0; while ((1u << bits) < ht_cap) ++bits;
        ht_shift = 32 - bits;
        ht = (HTEntry*)std::malloc((size_t)ht_cap * sizeof(HTEntry));
        if (!ht) fatal("BufferedStore: out of memory (hash table)");
        for (uint32_t i = 0; i < ht_cap; ++i) ht[i].page_no = NIL;
        file.open_new(path);
    }
    BufferedStore(const BufferedStore&) = delete;
    BufferedStore& operator=(const BufferedStore&) = delete;
    ~BufferedStore() { flush(); std::free(frames); std::free(ht); }

    Page* get(uint32_t pno) {
        int64_t fi = ht_find(pno);
        if (fi >= 0) {
            io.buf_hits++;
            if (lru_head != (uint32_t)fi) { lru_remove((uint32_t)fi); lru_push_front((uint32_t)fi); }
            return reinterpret_cast<Page*>(frames[fi].data);
        }
        io.buf_misses++;
        uint32_t f = grab_frame();
        file.read_page(pno, frames[f].data);
        frames[f].page_no = pno; frames[f].valid = 1; frames[f].dirty = 0;
        ht_insert(pno, f);
        lru_push_front(f);
        return reinterpret_cast<Page*>(frames[f].data);
    }
    void mark_dirty(uint32_t pno) {
        int64_t fi = ht_find(pno);
        if (fi < 0) fatal("BufferedStore::mark_dirty on a page that is not cached");
        frames[fi].dirty = 1;
    }
    uint32_t alloc() {                               // new zero page lives in the pool, dirty
        uint32_t pno = total_pages++;
        uint32_t f = grab_frame();
        std::memset(frames[f].data, 0, PAGE_SIZE);
        frames[f].page_no = pno; frames[f].valid = 1; frames[f].dirty = 1;
        ht_insert(pno, f);
        lru_push_front(f);
        return pno;
    }
    void flush() {
        for (uint32_t i = 0; i < used_frames; ++i)
            if (frames[i].valid && frames[i].dirty) { file.write_page(frames[i].page_no, frames[i].data); frames[i].dirty = 0; }
    }
    void sync() { flush(); file.sync(); }
    // Empty the pool (after flushing) → next accesses are all misses ("cold cache").
    void drop_caches() {
        flush();
        for (uint32_t i = 0; i < ht_cap; ++i) ht[i].page_no = NIL;
        for (uint32_t i = 0; i < used_frames; ++i) { frames[i].valid = 0; frames[i].lru_prev = frames[i].lru_next = NIL; }
        used_frames = 0; lru_head = lru_tail = NIL;
        file.drop_os_cache();
    }
    uint64_t file_size() const { return (uint64_t)total_pages * PAGE_SIZE; }
    uint64_t mem_bytes() const { return (uint64_t)pool_cap * sizeof(Frame) + (uint64_t)ht_cap * sizeof(HTEntry); }
};

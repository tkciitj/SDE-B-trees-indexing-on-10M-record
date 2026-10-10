#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// B+ Tree implemented from scratch (no external index library).
//
// The tree is *page based* and storage-agnostic: it only talks to a `Store`
// through   Page* get(pno)   /   void mark_dirty(pno)   /   uint32_t alloc().
// The same tree code therefore runs on top of
//   MemStore      – everything in RAM
//   DiskStore     – every page access goes to the file (no cache)
//   BufferedStore – LRU buffer pool in RAM + file (memory + disk)
//
// RULE obeyed by all code below: a `Page*` returned by store.get() is never
// used after another store.get()/store.alloc() call (the store may recycle the
// frame).  Page *numbers* are what is kept across calls.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <cassert>

// Compile-time configuration (override with -DBPT_KEY_LEN=16 etc.)
#ifndef BPT_PAGE_SIZE
#define BPT_PAGE_SIZE 4096
#endif
#ifndef BPT_KEY_LEN
#define BPT_KEY_LEN 8
#endif
#ifndef BPT_PAYLOAD_LEN
#define BPT_PAYLOAD_LEN 64
#endif

static constexpr uint32_t PAGE_SIZE    = BPT_PAGE_SIZE;
static constexpr uint32_t KEY_LEN      = BPT_KEY_LEN;
static constexpr uint32_t PAYLOAD_LEN  = BPT_PAYLOAD_LEN;
static constexpr uint32_t INVALID_PAGE = 0xFFFFFFFFu;

// Node header: is_leaf(1) + pad(1) + num_keys(2) + next_leaf(4) = 8 bytes
static constexpr uint32_t HEADER_SIZE = 8;
static constexpr uint32_t BODY_SIZE   = PAGE_SIZE - HEADER_SIZE;

// Internal node: children[M+1] (4 B each) + keys[M] (KEY_LEN each)
//   BODY >= 4*(M+1) + KEY_LEN*M   =>   M = (BODY-4) / (KEY_LEN+4)
static constexpr uint32_t INTERNAL_ENTRY = KEY_LEN + 4;
static constexpr uint32_t MAX_IKEYS      = (BODY_SIZE - 4) / INTERNAL_ENTRY;

// Leaf node: keys[L] + payloads[L]
static constexpr uint32_t RECORD_SIZE = KEY_LEN + PAYLOAD_LEN;
static constexpr uint32_t MAX_LRECS   = BODY_SIZE / RECORD_SIZE;

static_assert(MAX_IKEYS >= 4 && MAX_LRECS >= 3, "page too small for this key/payload size");
static_assert(BODY_SIZE % 4 == 0, "PAGE_SIZE must be a multiple of 4");

// ── Page layouts (every page is exactly PAGE_SIZE bytes) ────────────────────
// Page 0 holds the metadata (only the first sizeof(MetaPage) bytes are used).
struct MetaPage {
    uint32_t root_page;
    uint32_t height;          // 1 = root is a leaf
    uint64_t record_count;
    uint32_t total_pages;     // pages allocated, including page 0
    uint32_t key_len;
    uint32_t payload_len;
    uint32_t page_size;
    uint32_t leaf_nodes;
    uint32_t internal_nodes;
};
static_assert(sizeof(MetaPage) <= PAGE_SIZE, "meta must fit in a page");

struct NodePage {
    uint8_t  is_leaf;
    uint8_t  pad0;
    uint16_t num_keys;
    uint32_t next_leaf;       // leaf only: right sibling (INVALID_PAGE at the end)
    union {
        struct {              // internal node
            uint32_t children[MAX_IKEYS + 1];
            char     keys[MAX_IKEYS][KEY_LEN];
        } in;
        struct {              // leaf node
            char keys[MAX_LRECS][KEY_LEN];
            char payloads[MAX_LRECS][PAYLOAD_LEN];
        } lf;
        char raw[BODY_SIZE];
    } body;
};
static_assert(sizeof(NodePage) == PAGE_SIZE, "NodePage must be exactly one page");
using Page = NodePage;

// ── Key helpers ─────────────────────────────────────────────────────────────
// Keys are fixed-width, zero-padded byte strings compared with memcmp
// (= lexicographic order; a shorter key sorts before its extensions).
inline int keycmp(const char* a, const char* b) { return std::memcmp(a, b, KEY_LEN); }

// Internal node: child index to follow = number of separator keys <= key.
inline uint32_t internal_find_child(const NodePage* n, const char* key) {
    uint32_t lo = 0, hi = n->num_keys;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (keycmp(key, n->body.in.keys[mid]) < 0) hi = mid; else lo = mid + 1;
    }
    return lo;
}

// Leaf: first slot whose key >= `key` (lower bound).
inline uint32_t leaf_find_slot(const NodePage* n, const char* key) {
    uint32_t lo = 0, hi = n->num_keys;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (keycmp(n->body.lf.keys[mid], key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

struct SplitResult {
    bool     did_split = false;
    char     up_key[KEY_LEN];
    uint32_t right_page = INVALID_PAGE;
};

// ── B+ Tree ─────────────────────────────────────────────────────────────────
template <class Store>
class BPlusTree {
    Store&   store;
    MetaPage meta_;           // in-RAM copy of the 40-byte header; written to page 0 by sync()

    uint32_t alloc_page() {
        meta_.total_pages++;
        return store.alloc();
    }

    // ── leaf insert ────────────────────────────────────────────────────────
    SplitResult insert_leaf(uint32_t page_no, const char* key, const char* payload, bool& inserted) {
        SplitResult sr;
        Page* node = store.get(page_no);
        uint32_t n    = node->num_keys;
        uint32_t slot = leaf_find_slot(node, key);

        if (slot < n && keycmp(node->body.lf.keys[slot], key) == 0) {   // duplicate → overwrite
            std::memcpy(node->body.lf.payloads[slot], payload, PAYLOAD_LEN);
            store.mark_dirty(page_no);
            inserted = false;
            return sr;
        }
        inserted = true;

        if (n < MAX_LRECS) {                                            // room in leaf
            std::memmove(node->body.lf.keys[slot + 1],     node->body.lf.keys[slot],     (n - slot) * KEY_LEN);
            std::memmove(node->body.lf.payloads[slot + 1], node->body.lf.payloads[slot], (n - slot) * PAYLOAD_LEN);
            std::memcpy(node->body.lf.keys[slot],     key,     KEY_LEN);
            std::memcpy(node->body.lf.payloads[slot], payload, PAYLOAD_LEN);
            node->num_keys = (uint16_t)(n + 1);
            store.mark_dirty(page_no);
            return sr;
        }

        // Leaf full: build the MAX_LRECS+1 sorted records in a temp area, split in two.
        char tk[MAX_LRECS + 1][KEY_LEN];
        char tp[MAX_LRECS + 1][PAYLOAD_LEN];
        std::memcpy(tk, node->body.lf.keys,     n * KEY_LEN);
        std::memcpy(tp, node->body.lf.payloads, n * PAYLOAD_LEN);
        std::memmove(tk[slot + 1], tk[slot], (n - slot) * KEY_LEN);
        std::memmove(tp[slot + 1], tp[slot], (n - slot) * PAYLOAD_LEN);
        std::memcpy(tk[slot], key,     KEY_LEN);
        std::memcpy(tp[slot], payload, PAYLOAD_LEN);
        const uint32_t old_next = node->next_leaf;

        const uint32_t total = MAX_LRECS + 1;
        const uint32_t left_count  = total / 2;
        const uint32_t right_count = total - left_count;

        const uint32_t right_pno = alloc_page();
        meta_.leaf_nodes++;

        Page* right = store.get(right_pno);
        right->is_leaf   = 1;
        right->num_keys  = (uint16_t)right_count;
        right->next_leaf = old_next;
        std::memcpy(right->body.lf.keys,     tk[left_count], right_count * KEY_LEN);
        std::memcpy(right->body.lf.payloads, tp[left_count], right_count * PAYLOAD_LEN);
        store.mark_dirty(right_pno);

        node = store.get(page_no);
        node->num_keys  = (uint16_t)left_count;
        node->next_leaf = right_pno;
        std::memcpy(node->body.lf.keys,     tk, left_count * KEY_LEN);
        std::memcpy(node->body.lf.payloads, tp, left_count * PAYLOAD_LEN);
        store.mark_dirty(page_no);

        sr.did_split  = true;                         // leaf split: key is COPIED up
        sr.right_page = right_pno;
        std::memcpy(sr.up_key, tk[left_count], KEY_LEN);
        return sr;
    }

    // ── internal insert; `level` = distance to the leaves (1 → children are leaves)
    SplitResult insert_internal(uint32_t page_no, uint32_t level, const char* key,
                                const char* payload, bool& inserted) {
        SplitResult sr;
        Page* node = store.get(page_no);
        const uint32_t idx   = internal_find_child(node, key);
        const uint32_t child = node->body.in.children[idx];

        SplitResult cs = (level == 1) ? insert_leaf(child, key, payload, inserted)
                                      : insert_internal(child, level - 1, key, payload, inserted);
        if (!cs.did_split) return sr;

        // Child split → add (cs.up_key, cs.right_page) after position idx.
        node = store.get(page_no);                    // re-fetch: frame may have been recycled
        const uint32_t nk = node->num_keys;

        if (nk < MAX_IKEYS) {
            std::memmove(node->body.in.keys[idx + 1],   node->body.in.keys[idx],   (nk - idx) * KEY_LEN);
            std::memmove(&node->body.in.children[idx + 2], &node->body.in.children[idx + 1], (nk - idx) * sizeof(uint32_t));
            std::memcpy(node->body.in.keys[idx], cs.up_key, KEY_LEN);
            node->body.in.children[idx + 1] = cs.right_page;
            node->num_keys = (uint16_t)(nk + 1);
            store.mark_dirty(page_no);
            return sr;
        }

        // Internal node full: MAX_IKEYS+1 keys / MAX_IKEYS+2 children, middle key MOVES up.
        char     tk[MAX_IKEYS + 1][KEY_LEN];
        uint32_t tc[MAX_IKEYS + 2];
        std::memcpy(tk, node->body.in.keys,     MAX_IKEYS * KEY_LEN);
        std::memcpy(tc, node->body.in.children, (MAX_IKEYS + 1) * sizeof(uint32_t));
        std::memmove(tk[idx + 1], tk[idx], (MAX_IKEYS - idx) * KEY_LEN);
        std::memmove(&tc[idx + 2], &tc[idx + 1], (MAX_IKEYS - idx) * sizeof(uint32_t));
        std::memcpy(tk[idx], cs.up_key, KEY_LEN);
        tc[idx + 1] = cs.right_page;

        const uint32_t total       = MAX_IKEYS + 1;
        const uint32_t mid         = total / 2;
        const uint32_t left_count  = mid;
        const uint32_t right_count = total - mid - 1;

        const uint32_t right_pno = alloc_page();
        meta_.internal_nodes++;

        Page* right = store.get(right_pno);
        right->is_leaf   = 0;
        right->num_keys  = (uint16_t)right_count;
        right->next_leaf = INVALID_PAGE;
        std::memcpy(right->body.in.keys,     tk[mid + 1],  right_count * KEY_LEN);
        std::memcpy(right->body.in.children, &tc[mid + 1], (right_count + 1) * sizeof(uint32_t));
        store.mark_dirty(right_pno);

        node = store.get(page_no);
        node->num_keys = (uint16_t)left_count;
        std::memcpy(node->body.in.keys,     tk, left_count * KEY_LEN);
        std::memcpy(node->body.in.children, tc, (left_count + 1) * sizeof(uint32_t));
        store.mark_dirty(page_no);

        sr.did_split  = true;
        sr.right_page = right_pno;
        std::memcpy(sr.up_key, tk[mid], KEY_LEN);
        return sr;
    }

    // ── structural verification helpers ───────────────────────────────────
    // level 0 = leaf. Keys of the subtree must lie in [lo, hi) (nullptr = unbounded).
    bool check_node(uint32_t pno, uint32_t level, const char* lo, const char* hi, uint64_t& records) {
        Page* node = store.get(pno);
        const uint32_t nk = node->num_keys;
        if (level == 0) {
            if (!node->is_leaf || nk > MAX_LRECS) return false;
            for (uint32_t i = 0; i < nk; ++i) {
                const char* k = node->body.lf.keys[i];
                if (i > 0 && keycmp(node->body.lf.keys[i - 1], k) >= 0) return false;
                if (lo && keycmp(k, lo) < 0) return false;
                if (hi && keycmp(k, hi) >= 0) return false;
            }
            records += nk;
            return true;
        }
        if (node->is_leaf || nk < 1 || nk > MAX_IKEYS) return false;
        char     keys[MAX_IKEYS][KEY_LEN];
        uint32_t ch[MAX_IKEYS + 1];
        std::memcpy(keys, node->body.in.keys, nk * KEY_LEN);
        std::memcpy(ch, node->body.in.children, (nk + 1) * sizeof(uint32_t));
        for (uint32_t i = 0; i < nk; ++i) {
            if (i > 0 && keycmp(keys[i - 1], keys[i]) >= 0) return false;
            if (lo && keycmp(keys[i], lo) < 0) return false;
            if (hi && keycmp(keys[i], hi) >= 0) return false;
        }
        for (uint32_t i = 0; i <= nk; ++i) {
            const char* clo = (i == 0)  ? lo : keys[i - 1];
            const char* chi = (i == nk) ? hi : keys[i];
            if (!check_node(ch[i], level - 1, clo, chi, records)) return false;
        }
        return true;
    }

public:
    explicit BPlusTree(Store& s) : store(s) {
        std::memset(&meta_, 0, sizeof(meta_));
        uint32_t p0 = store.alloc();                 // page 0 = metadata
        uint32_t p1 = store.alloc();                 // page 1 = empty root leaf
        assert(p0 == 0 && p1 == 1); (void)p0; (void)p1;
        Page* root = store.get(1);
        root->is_leaf   = 1;
        root->num_keys  = 0;
        root->next_leaf = INVALID_PAGE;
        store.mark_dirty(1);

        meta_.root_page    = 1;
        meta_.height       = 1;
        meta_.total_pages  = 2;
        meta_.key_len      = KEY_LEN;
        meta_.payload_len  = PAYLOAD_LEN;
        meta_.page_size    = PAGE_SIZE;
        meta_.leaf_nodes   = 1;
        sync();
    }

    // Write the header to page 0.  Called automatically when the root changes;
    // call it once more after the last insert so page 0 is current on disk.
    void sync() {
        Page* p = store.get(0);
        std::memcpy(p, &meta_, sizeof(MetaPage));
        store.mark_dirty(0);
    }

    // Returns true if a new key was added, false if an existing key was overwritten.
    bool insert(const char* key, const char* payload) {
        bool inserted = false;
        SplitResult sr = (meta_.height == 1)
            ? insert_leaf(meta_.root_page, key, payload, inserted)
            : insert_internal(meta_.root_page, meta_.height - 1, key, payload, inserted);

        if (inserted) meta_.record_count++;

        if (sr.did_split) {                           // root split → tree grows by one level
            const uint32_t old_root = meta_.root_page;
            const uint32_t new_root = alloc_page();
            meta_.internal_nodes++;
            Page* nr = store.get(new_root);
            nr->is_leaf   = 0;
            nr->num_keys  = 1;
            nr->next_leaf = INVALID_PAGE;
            nr->body.in.children[0] = old_root;
            nr->body.in.children[1] = sr.right_page;
            std::memcpy(nr->body.in.keys[0], sr.up_key, KEY_LEN);
            store.mark_dirty(new_root);
            meta_.root_page = new_root;
            meta_.height++;
            sync();
        }
        return inserted;
    }

    // Point lookup. Copies the payload to `out` (if non-null). Reads `height` pages.
    bool search(const char* key, char* out = nullptr) {
        uint32_t cur = meta_.root_page;
        for (uint32_t level = 1; level < meta_.height; ++level) {
            Page* node = store.get(cur);
            cur = node->body.in.children[internal_find_child(node, key)];
        }
        Page* leaf = store.get(cur);
        uint32_t slot = leaf_find_slot(leaf, key);
        if (slot < leaf->num_keys && keycmp(leaf->body.lf.keys[slot], key) == 0) {
            if (out) std::memcpy(out, leaf->body.lf.payloads[slot], PAYLOAD_LEN);
            return true;
        }
        return false;
    }

    // Range query helper (uses the leaf chain): number of keys k with lo <= k < hi.
    uint64_t range_count(const char* lo, const char* hi) {
        uint32_t cur = meta_.root_page;
        for (uint32_t level = 1; level < meta_.height; ++level) {
            Page* node = store.get(cur);
            cur = node->body.in.children[internal_find_child(node, lo)];
        }
        uint64_t count = 0;
        uint32_t slot = 0;
        bool first = true;
        while (cur != INVALID_PAGE) {
            Page* leaf = store.get(cur);
            if (first) { slot = leaf_find_slot(leaf, lo); first = false; }
            for (; slot < leaf->num_keys; ++slot) {
                if (keycmp(leaf->body.lf.keys[slot], hi) >= 0) return count;
                ++count;
            }
            slot = 0;
            cur = leaf->next_leaf;
        }
        return count;
    }

    // Full verification: key ordering/ranges in every node, all leaves on the same
    // level, leaf chain sorted, record counts consistent.
    bool verify(uint64_t* records_out = nullptr) {
        uint64_t recs = 0;
        if (!check_node(meta_.root_page, meta_.height - 1, nullptr, nullptr, recs)) return false;

        uint32_t cur = meta_.root_page;
        for (uint32_t level = 1; level < meta_.height; ++level)
            cur = store.get(cur)->body.in.children[0];
        char prev[KEY_LEN]; bool has_prev = false;
        uint64_t chain = 0, leaves = 0;
        while (cur != INVALID_PAGE) {
            Page* leaf = store.get(cur);
            ++leaves;
            for (uint32_t i = 0; i < leaf->num_keys; ++i) {
                if (has_prev && keycmp(prev, leaf->body.lf.keys[i]) >= 0) return false;
                std::memcpy(prev, leaf->body.lf.keys[i], KEY_LEN);
                has_prev = true;
                ++chain;
            }
            cur = leaf->next_leaf;
        }
        if (records_out) *records_out = chain;
        return chain == recs && chain == meta_.record_count && leaves == meta_.leaf_nodes;
    }

    uint32_t height()         const { return meta_.height; }
    uint64_t record_count()   const { return meta_.record_count; }
    uint32_t leaf_nodes()     const { return meta_.leaf_nodes; }
    uint32_t internal_nodes() const { return meta_.internal_nodes; }
    uint32_t total_pages()    const { return meta_.total_pages; }
};

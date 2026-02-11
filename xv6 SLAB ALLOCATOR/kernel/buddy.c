#include "buddy.h"

void **free_area;
block_meta_t *block_meta;

uintptr_t managed_start;
int managed_num_blocks;

struct {
  struct spinlock lock;
} buddy;



void buddy_init() {
    for (int o = 0; o <= MAX_ORDER; o++)
        free_area[o] = NULL;


    for (int i = 0; i < managed_num_blocks; i++) {
        block_meta[i].is_head_of_free_block = 0; //not a head of a free block
        block_meta[i].free_order = 0;

		block_meta[i].is_head_of_alloc_block = 0;
   		block_meta[i].alloc_order = 0;

		block_meta[i].owner_slab = NULL;
    }

    int curr = 0;
    while (curr < managed_num_blocks) {
        int remaining = managed_num_blocks - curr;

        // pick largest order that fits and is aligned at curr
        int order = MAX_ORDER;
        while (order > 0) {
            int block_pages = 1 << order;

            if (block_pages <= remaining && (curr % block_pages) == 0)
                break;

            order--;
        }

        int block_pages = 1 << order;

        // mark metadata only at head
        block_meta[curr].is_head_of_free_block = 1;
        block_meta[curr].free_order = (uint8_t) order;

        // compute address of this block head
        void *addr = (void*)(managed_start + (uintptr_t)curr * BLOCK_SIZE);

        // add to freelist for that order
        push_free(order, addr);

        curr += block_pages;
    }



}

static inline void push_free(int order, void *blk) {
    *(void**)blk = free_area[order];
    free_area[order] = blk;
}

static inline void *pop_free(int order) {
    void *blk = free_area[order];
    if (!blk) return NULL;
    free_area[order] = *(void**)blk;

    int pop_idx = addr_to_index(blk);
    if (pop_idx >= 0) block_meta[pop_idx].is_head_of_free_block = 0;

    return blk;
}

static inline uintptr_t buddy_of(uintptr_t addr, size_t bytes) {
    return addr ^ bytes; // XOR flip the bit for that chunk size
}

void* buddy_alloc(size_t size_in_pages)
{
    if (size_in_pages == 0) return NULL;

    int target = order_for_pages(size_in_pages);
    if (target > MAX_ORDER) return NULL;

    int k = target;
    while (k <= MAX_ORDER && free_area[k] == NULL) k++;
    if (k > MAX_ORDER) return NULL;

    void *blk = pop_free(k);

    // blk was the head of a FREE block; it's not free anymore
    mark_not_free_head(blk);

    while (k > target) {
        k--;
        size_t chunk_bytes = (size_t)BLOCK_SIZE << k;

        void *buddy = (void *)((char*)blk + chunk_bytes);

        // buddy becomes a FREE head block of order k
        mark_free_head(buddy, k);
        push_free(k, buddy);

        // blk remains the first half, still allocated, so keep it not-free
        // (no extra metadata needed)
    }


	int idx = addr_to_index(blk);
	block_meta[idx].is_head_of_alloc_block = 1;
	block_meta[idx].alloc_order = target;

	block_meta[idx].is_head_of_free_block = 0;

    return blk;
}

int order_for_pages(size_t size_in_pages) {
    int initialOrder = 0;
    int initialNumOfPages = 1;
    while (initialNumOfPages < size_in_pages) {
        initialOrder++;
        initialNumOfPages *= 2;
    }
    return initialOrder;
}

static inline void mark_free_head(void *addr, int order) {
    int idx = addr_to_index(addr);
    block_meta[idx].is_head_of_free_block = 1;
    block_meta[idx].free_order = (uint8_t)order;
}

static inline void mark_not_free_head(void *addr) {
    int idx = addr_to_index(addr);
    block_meta[idx].is_head_of_free_block = 0;
    // block_meta[idx].free_order can be left as-is (ignored when allocated)
}

static int remove_from_freelist(int order, void *blk) {
    void *prev = NULL;
    void *cur  = free_area[order];

    while (cur) {
        void *next = *(void**)cur;
        if (cur == blk) {
            if (prev) *(void**)prev = next;
            else      free_area[order] = next;
            return 1; // removed
        }
        prev = cur;
        cur = next;
    }
    return 0; // not found
}

void buddy_free(void *ptr) {
    if (!ptr) return;

    int idx = addr_to_index(ptr);
    if (idx < 0) return;

    // must be an allocated head and not slab-owned
    if (!block_meta[idx].is_head_of_alloc_block || block_meta[idx].owner_slab != NULL) {
        // panic or return
        return;
    }

    int order = block_meta[idx].alloc_order;

    // clear alloc-head flag immediately (prevents double free)
    block_meta[idx].is_head_of_alloc_block = 0;

    buddy_free_order(ptr, order);   // your existing merge logic version
}


static void buddy_free_order(void *ptr, int order) {
    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;

    if ((addr - managed_start) % BLOCK_SIZE != 0) {
        return; // panic
    }

    int cur = order;

    while (cur < MAX_ORDER) {
        size_t bytes = (size_t)BLOCK_SIZE << cur;
        uintptr_t buddy = addr ^ bytes;

        int bidx = addr_to_index((void*)buddy);
        if (bidx < 0) break;

        if (!(block_meta[bidx].is_head_of_free_block &&
			block_meta[bidx].free_order == cur &&
			block_meta[bidx].owner_slab == NULL &&
			block_meta[bidx].is_head_of_alloc_block == 0)) {
            break;
        }

        if (!remove_from_freelist(cur, (void*)buddy)) {
            break;
        }

        // IMPORTANT: clear stale head mark on buddy we merged away
        block_meta[bidx].is_head_of_free_block = 0;

        if (buddy < addr) addr = buddy;
        cur++;
    }

    int idx = addr_to_index((void*)addr);
    block_meta[idx].is_head_of_free_block = 1;
    block_meta[idx].free_order = (uint8_t)cur;

    push_free(cur, (void*)addr);
}

/*static inline int get_order(void* ptr) {
    int index = addr_to_index(ptr);
    block_meta_t m = block_meta[idx]
    return m.free_order;
}*/


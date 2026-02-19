#include "buddy.h"

#include "defs.h"

#include "proc.h"



kmem_state_t kmem; //we have to use a global for kernel memory





static inline kmem_state_t*
curr_kmem(void)
{
  struct cpu *c = mycpu();
  if(c && c->active_kmem)
    return c->active_kmem;
  return &kmem;
}


static inline void push_free(int order, void *blk) {
	kmem_state_t* my_kmem = curr_kmem();
    *(void**)blk = my_kmem->free_area[order];
    my_kmem->free_area[order] = blk;
}

static inline void *pop_free(int order) {
	kmem_state_t* my_kmem = curr_kmem();
    void *blk = my_kmem->free_area[order];
    if (!blk) return NULL;
    my_kmem->free_area[order] = *(void**)blk;

    int pop_idx = addr_to_index(blk);
    if (pop_idx >= 0) my_kmem->block_meta[pop_idx].is_head_of_free_block = 0;

    return blk;
}

static inline uintptr_t buddy_of(uintptr_t addr, uint64 bytes) {
    return addr ^ bytes; // XOR flip the bit for that chunk size
}

static inline void mark_free_head(void *addr, int order) {
	kmem_state_t* my_kmem = curr_kmem();
    int idx = addr_to_index(addr);
    my_kmem->block_meta[idx].is_head_of_free_block = 1;
    my_kmem->block_meta[idx].free_order = (uint8)order;
}

static inline void mark_not_free_head(void *addr) {
	kmem_state_t* my_kmem = curr_kmem();
    int idx = addr_to_index(addr);
    my_kmem->block_meta[idx].is_head_of_free_block = 0;
}

static int remove_from_freelist(int order, void *blk) {
	kmem_state_t* my_kmem = curr_kmem();
    void *prev = NULL;
    void *cur  = my_kmem->free_area[order];

    while (cur) {
        void *next = *(void**)cur;
        if (cur == blk) {
            if (prev) *(void**)prev = next;
            else      my_kmem->free_area[order] = next;
            return 1; // removed
        }
        prev = cur;
        cur = next;
    }
    return 0; // not found
}

int order_for_pages(uint64 size_in_pages) {
    int initialOrder = 0;
    int initialNumOfPages = 1;
    while (initialNumOfPages < size_in_pages) {
        initialOrder++;
        initialNumOfPages *= 2;
    }
    return initialOrder;
}



void buddy_init() {
	kmem_state_t* my_kmem = curr_kmem();

    initlock(&my_kmem->buddy_lock, "buddy");

    for (int o = 0; o <= MAX_ORDER; o++)
        my_kmem->free_area[o] = NULL;


    for (int i = 0; i < my_kmem->managed_num_blocks; i++) {
        my_kmem->block_meta[i].is_head_of_free_block = 0; //not a head of a free block
        my_kmem->block_meta[i].free_order = 0;

		my_kmem->block_meta[i].is_head_of_alloc_block = 0;
   		my_kmem->block_meta[i].alloc_order = 0;

		my_kmem->block_meta[i].owner_slab = NULL;
    }

    int curr = 0;
    while (curr < my_kmem->managed_num_blocks) {
        int remaining = my_kmem->managed_num_blocks - curr;

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
        my_kmem->block_meta[curr].is_head_of_free_block = 1;
        my_kmem->block_meta[curr].free_order = (uint8) order;

        // compute address of this block head
        void *addr = (void*)(my_kmem->managed_start + (uintptr_t)curr * BLOCK_SIZE);

        // add to freelist for that order
        push_free(order, addr);

        curr += block_pages;
    }



}



void* buddy_alloc(uint64 size_in_pages)
{
	kmem_state_t* my_kmem = curr_kmem();

    if (size_in_pages == 0) return NULL;

    acquire(&my_kmem->buddy_lock);

    int target = order_for_pages(size_in_pages);
    if (target > MAX_ORDER) {release(&my_kmem->buddy_lock); return NULL;}

    int k = target;
    while (k <= MAX_ORDER && my_kmem->free_area[k] == NULL) k++;
    if (k > MAX_ORDER) {release(&my_kmem->buddy_lock); return NULL;}

    void *blk = pop_free(k);

    // blk was the head of a FREE block; it's not free anymore
    mark_not_free_head(blk);

    while (k > target) {
        k--;
        uint64 chunk_bytes = (uint64)BLOCK_SIZE << k;

        void *buddy = (void *)((char*)blk + chunk_bytes);

        // buddy becomes a FREE head block of order k
        mark_free_head(buddy, k);
        push_free(k, buddy);

        // blk remains the first half, still allocated, so keep it not-free
        // (no extra metadata needed)
    }


	int idx = addr_to_index(blk);
	my_kmem->block_meta[idx].is_head_of_alloc_block = 1;
	my_kmem->block_meta[idx].alloc_order = target;

	my_kmem->block_meta[idx].is_head_of_free_block = 0;


    release(&my_kmem->buddy_lock);
    return blk;
}

int addr_to_index(void *ptr) {
  kmem_state_t* my_kmem = curr_kmem();
  uintptr_t a = (uintptr_t)ptr;

  if (a < (uintptr_t)my_kmem->managed_start) return -1;
  uintptr_t off = a - (uintptr_t)my_kmem->managed_start;
  if (off % BLOCK_SIZE != 0) return -1;

  int idx = (int)(off / BLOCK_SIZE);
  if (idx < 0 || idx >= my_kmem->managed_num_blocks) return -1;
  return idx;
}



static void buddy_free_order(void *ptr, int order) {
	kmem_state_t* my_kmem = curr_kmem();

    //lock must be held, if somebody implements a function that calls this one, it is not thread safe

    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;

    if ((addr - my_kmem->managed_start) % BLOCK_SIZE != 0) {
        return; // panic
    }

    int cur = order;

    while (cur < MAX_ORDER) {
        uint64 bytes = (uint64)BLOCK_SIZE << cur;
        uintptr_t buddy = addr ^ bytes;

        int bidx = addr_to_index((void*)buddy);
        if (bidx < 0) break;

        if (!(my_kmem->block_meta[bidx].is_head_of_free_block &&
            my_kmem->block_meta[bidx].free_order == cur &&
            my_kmem->block_meta[bidx].owner_slab == NULL &&
            my_kmem->block_meta[bidx].is_head_of_alloc_block == 0)) {
            break;
            }

        if (!remove_from_freelist(cur, (void*)buddy)) {
            break;
        }

        // IMPORTANT: clear stale head mark on buddy we merged away
        my_kmem->block_meta[bidx].is_head_of_free_block = 0;

        if (buddy < addr) addr = buddy;
        cur++;
    }

    int idx = addr_to_index((void*)addr);
    my_kmem->block_meta[idx].is_head_of_free_block = 1;
    my_kmem->block_meta[idx].free_order = (uint8)cur;

    push_free(cur, (void*)addr);
}



void buddy_free(void *ptr) {
    if (!ptr) return;
	kmem_state_t* my_kmem = curr_kmem();

    acquire(&my_kmem->buddy_lock);

    int idx = addr_to_index(ptr);
    if (idx < 0) { release(&my_kmem->buddy_lock); return; }

    // must be an allocated head and not slab-owned
    if (!my_kmem->block_meta[idx].is_head_of_alloc_block || my_kmem->block_meta[idx].owner_slab != NULL) {
        // panic or return
        release(&my_kmem->buddy_lock);
        return;
    }

    int order = my_kmem->block_meta[idx].alloc_order;

    // clear alloc-head flag immediately (prevents double free)
    my_kmem->block_meta[idx].is_head_of_alloc_block = 0;

    buddy_free_order(ptr, order);   // your existing merge logic version

    release(&my_kmem->buddy_lock);

}




/*static inline int get_order(void* ptr) {
    int index = addr_to_index(ptr);
    block_meta_t m = block_meta[idx]
    return m.free_order;
}*/


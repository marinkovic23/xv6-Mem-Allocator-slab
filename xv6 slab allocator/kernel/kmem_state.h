
#ifndef _KMEM_STATE_H
#define _KMEM_STATE_H

#define KMEM_MAX_CACHES 128
#define KMEM_MAX_ORDER  15

struct kmem_state {
    // region
    uint64 managed_start;     // user VA base (page-aligned)
    int    managed_num_blocks;

    // buddy
    void **free_area;         // array [0..MAX_ORDER] of freelist heads (kernel-resident)
    block_meta_t *block_meta; // array [managed_num_blocks] (kernel-resident)
    struct spinlock buddy_lock;

    // slab/cache
    kmem_cache_t *cache_pool; // [KMEM_MAX_CACHES] kernel-resident descriptors
    struct spinlock cache_list_lock;
    kmem_cache_t *head_of_cache_list;
    kmem_cache_t *tail_of_cache_list;

    // kmalloc size caches
    kmem_cache_t *kmalloc_caches[13];
};



#endif
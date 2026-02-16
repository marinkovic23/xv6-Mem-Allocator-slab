#ifndef __BUDDY_H__
#define __BUDDY_H__

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"

//forward declaration
typedef struct kmem_cache_s kmem_cache_t;

//my modification defining NULL
#define NULL ((void*) 0)


typedef uint64 uintptr_t;

#define BLOCK_SIZE 4096


struct slab_s;
typedef struct slab_s slab_t;

typedef struct kmem_state kmem_state_t;


typedef struct {
    //block meaning one or more contiguous pages
    uint8 free_order;      // valid if this is head of a free block
    uint8 is_head_of_free_block;    // 1 if this page is the head of a free block, else 0


	uint8 alloc_order;              // valid if is_head_of_alloc_block
    uint8 is_head_of_alloc_block;   // 1 if allocated head (buddy allocation)

    slab_t* owner_slab;




} block_meta_t;

extern void** free_area;
extern block_meta_t* block_meta;

extern uintptr_t managed_start;
extern int managed_num_blocks;

#define MAX_ORDER 15

typedef struct {
    struct spinlock lock;
} buddy_state_t;

extern buddy_state_t buddy;

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


void buddy_init(struct kmem_state *ks); //

void* buddy_alloc(struct kmem_state *ks, uint64 size_in_pages);

void buddy_free(struct kmem_state *ks, void *ptr);

//helper that is used in both slab.c and buddy.c
static inline int addr_to_index(kmem_state_t *ks, void *ptr) {
  uintptr_t a = (uintptr_t)ptr;

  if (a < (uintptr_t)ks->managed_start) return -1;
  uintptr_t off = a - (uintptr_t)ks->managed_start;
  if (off % BLOCK_SIZE != 0) return -1;

  int idx = (int)(off / BLOCK_SIZE);
  if (idx < 0 || idx >= ks->managed_num_blocks) return -1;
  return idx;
}

















#endif
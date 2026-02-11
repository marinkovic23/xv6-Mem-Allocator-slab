#ifndef __BUDDY_H__
#define __BUDDY_H__

#include "types.h"


typedef uint64 uintptr_t;

#define BLOCK_SIZE 4096


struct slab_s;
typedef struct slab_s slab_t;

typedef struct {
    //block meaning one or more contiguous pages
    uint8_t free_order;      // valid if this is head of a free block
    uint8_t is_head_of_free_block;    // 1 if this page is the head of a free block, else 0


	uint8_t alloc_order;              // valid if is_head_of_alloc_block
    uint8_t is_head_of_alloc_block;   // 1 if allocated head (buddy allocation)

    slab_t* owner_slab;




} block_meta_t;

extern void** free_area;
extern block_meta_t* block_meta;

extern uintptr_t managed_start;
extern int managed_num_blocks;

#define MAX_ORDER 15

void buddy_init(); //

void* buddy_alloc(size_t size_in_pages);

void buddy_free(void *ptr);

//helper that is used in both slab.c and buddy.c
static inline int addr_to_index(void* ptr) {
	uintptr_t a = (uintptr_t)ptr;

    // optional safety checks:
    if (a < managed_start) return -1;
    uintptr_t off = a - managed_start;
    if (off % BLOCK_SIZE != 0) return -1; // must be page-aligned
    int idx = (int)(off / BLOCK_SIZE);
    if (idx < 0 || idx >= managed_num_blocks) return -1;

    return idx;
}

















#endif
#ifndef __slab_h__
#define __slab_h__

#include "buddy.h"


//this is the start of the part that I'm adding to this file

extern void* heap_base;
extern int block_num;
#define MAX_NUM_CACHES 128
#define KMALLOC_NUM_CACHES 11


extern kmem_cache_t* cache_pool;

extern kmem_cache_t* head_of_cache_list;
extern kmem_cache_t* tail_of_cache_list;

extern int cache_pool_used;

void add_to_cache_list(kmem_cache_t* newCache);
kmem_cache_t* remove_from_cache_list();

#define MAX_SLAB_ORDER 3 //8 pages is a conservative default
#define MIN_OBJS_PER_SLAB 8





//this is the end of my additions

typedef struct kmem_cache_s kmem_cache_t;


#define BLOCK_SIZE 4096
typedef unsigned long size_t;
void kmem_init(void *space, int block_num);
kmem_cache_t *kmem_cache_create(const char *name, size_t size,
void (*ctor)(void *),
void (*dtor)(void *)); // Allocate cache
int kmem_cache_shrink(kmem_cache_t *cachep); // Shrink cache
void *kmem_cache_alloc(kmem_cache_t *cachep); // Allocate one object from cache
void kmem_cache_free(kmem_cache_t *cachep, void *objp); // Deallocate one object from cache
void *kmalloc(size_t size); // Alloacate one small memory buffer
void kfree(const void *objp); // Deallocate one small memory buffer
void kmem_cache_destroy(kmem_cache_t *cachep); // Deallocate cache
void kmem_cache_info(kmem_cache_t *cachep); // Print cache info
int kmem_cache_error(kmem_cache_t *cachep); // Print error message


#endif
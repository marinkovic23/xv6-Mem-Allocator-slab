#ifndef __slab_h__
#define __slab_h__

#include "buddy.h"

typedef struct kmem_cache_s kmem_cache_t; //forward decl


//this is the start of the part that I'm adding to this file

#define MAX_NUM_CACHES 128
#define KMALLOC_NUM_CACHES 13



void add_to_cache_list(kmem_cache_t* newCache);
kmem_cache_t* remove_from_cache_list(kmem_cache_t* cachep);

#define MAX_SLAB_ORDER 6 //8 pages is a conservative default
#define MIN_OBJS_PER_SLAB 8


extern uint64 kheapwin_base0;   // global
extern uint64 kheapwin_total;   // global (NPROC * KHEAPWIN_SIZE)


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
void k_free(const void *objp); // Deallocate one small memory buffer
void kmem_cache_destroy(kmem_cache_t *cachep); // Deallocate cache
void kmem_cache_info(kmem_cache_t *cachep); // Print cache info
int kmem_cache_error(kmem_cache_t *cachep); // Print error message


void my_kinit(); //my init function

#endif
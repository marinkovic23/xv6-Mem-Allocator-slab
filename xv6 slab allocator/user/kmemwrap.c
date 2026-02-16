#include "user.h"

#define MAXC 128
static uint64 handles[MAXC];
static void (*ctors[MAXC])(void*);
static void (*dtors[MAXC])(void*);

static int findslot(uint64 h){
    for(int i=0;i<MAXC;i++) if(handles[i]==h) return i;
    return -1;
}
static int newslot(uint64 h){
    for(int i=0;i<MAXC;i++) if(handles[i]==0){ handles[i]=h; return i; }
    return -1;
}

void kmem_init(void *space, int block_num){
    k_kmem_init(space, block_num);
}

kmem_cache_t* kmem_cache_create(const char *name, int size, void (*ctor)(void*), void (*dtor)(void*)){
    uint64 h = (uint64)k_kmem_cache_create(name, size); // returns handle encoded as pointer
    if(h == 0) return 0;
    int s = newslot(h);
    if(s >= 0){ ctors[s]=ctor; dtors[s]=dtor; }
    return (kmem_cache_t*)h;
}

void* kmem_cache_alloc(kmem_cache_t *cache){
    void *p = (void*)k_kmem_cache_alloc(cache);
    if(!p) return 0;
    int s = findslot((uint64)cache);
    if(s >= 0 && ctors[s]) ctors[s](p);
    return p;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj){
    int s = findslot((uint64)cache);
    if(s >= 0 && dtors[s]) dtors[s](obj);
    k_kmem_cache_free(cache, obj);
}

void kmem_cache_info(kmem_cache_t *cache){
    k_kmem_cache_info(cache);
}

void kmem_cache_destroy(kmem_cache_t *cache){
    k_kmem_cache_destroy(cache);
    int s = findslot((uint64)cache);
    if(s >= 0){ handles[s]=0; ctors[s]=0; dtors[s]=0; }
}

void* kmalloc(int size){
    return (void*)k_kmalloc(size);
}

void kfree(void *p){
    k_kfree(p);
}

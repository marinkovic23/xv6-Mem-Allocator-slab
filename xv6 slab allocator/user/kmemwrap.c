#include "ucache.h"
#include "user.h"


kmem_cache_t*
kmem_cache_create(const char *name, int objsz, void (*ctor)(void*), void (*dtor)(void*))
{
    //printf("[wrap] create name=%s objsz=%d ctor=%p dtor=%p\n", name, objsz, ctor, dtor);

    uint64 kc = k_kmem_cache_create(name, objsz, 0, 0); // kernel ignores ctor/dtor
    if(kc == 0) return 0;

    if(insert(kc, objsz, ctor, dtor) == 0){
        // optional: destroy kernel cache if we can't track it
        k_kmem_cache_destroy(kc);
        return 0;
    }
    return (kmem_cache_t*)kc;  // still looks like a kmem_cache_t* to the test
}

void*
kmem_cache_alloc(kmem_cache_t *cache)
{
    if(cache == 0) return 0;
    uint64 kc = (uint64)cache;

    void *p = (void*)k_kmem_cache_alloc(kc);
    if(p == 0) return 0;

    struct ucent *e = find(kc);
    if(e && e->ctor) e->ctor(p);   // ctor runs in user mode
    return p;
}

uint64
kmem_cache_free(kmem_cache_t *cache, void *obj)
{
    if(cache == 0 || obj == 0) return -1;
    uint64 kc = (uint64)cache;

    struct ucent *e = find(kc);
    if(e && e->dtor) e->dtor(obj); // dtor runs in user mode

    uint64 return_value = k_kmem_cache_free(kc, (uint64)obj);
    return return_value;
}

uint64
kmem_cache_destroy(kmem_cache_t *cache)
{
    if(cache == 0) return -1;
    uint64 kc = (uint64)cache;

    uint64 return_value = k_kmem_cache_destroy(kc);
    remove_entry(kc);
    return return_value;
}
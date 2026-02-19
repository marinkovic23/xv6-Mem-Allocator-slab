#include "slab.h"




#include "defs.h"

#include "proc.h"




extern char end[];


uint64 kheapwin_base0;
int kheapwin_nwin;
uint64 kheapwin_total;   // global (NPROC * KHEAPWIN_SIZE)


typedef struct slab_s {
    kmem_cache_t* cache; // owner cache
	uint16 first_free; //index of first free object
	uint16 free_count;
    //void *free_list;            // head of free objects in this slab   maybe scrap this shit
    unsigned inuse;             // allocated objects count
    slab_t* next;
    slab_t* prev;
} slab_t;

#define NOFREE 0xFFFF

enum Error{
    KMEM_OK = 0,
    KMEM_ERR_NULL_CACHE,
    KMEM_ERR_NO_SPACE_IN_POOL,
    KMEM_ERR_BAD_OBJ_SIZE,
    KMEM_ERR_BUDDY_OOM,
    KMEM_ERR_INVALID_FREE_PTR,
    KMEM_ERR_CACHE_MISMATCH,
    KMEM_ERR_DOUBLE_FREE,
    KMEM_ERR_DESTROY_NOT_EMPTY,
    KMEM_ERR_DESTROY_KMALLOC_CACHE,
    KMEM_ERR_UNINITIALIZED_OBJECT
};


struct kmem_cache_s {
	struct spinlock lock;

    slab_t* full;
    slab_t* partial;
    slab_t* empty;


    //constructors and destructors for the objects held by this cache
    void (*ctor)(void*);
    void (*dtor)(void*);

    
    char* name;
    int slab_order; //one slab is 1<<slab_order pages
    size_t object_size; //aligned >= sizeof(void*)
    size_t objs_per_slab; //number of objects that fit on one slab
	size_t obj_offset; //from header to the first object


    //error code;
    enum Error error_code;



    uint8 is_used;

    uint8 destroying;

    //spinlock_t spinlock; will be done at some point

    kmem_cache_t* next;
    kmem_cache_t* prev;
};

static inline kmem_state_t*
curr_kmem(void)
{
  struct cpu *c = mycpu();
  if(c && c->active_kmem)
    return c->active_kmem;
  return &kmem;
}

static inline uintptr_t ALIGN_UP(uintptr_t addr, uintptr_t align) {
    uintptr_t remainder = addr % align;
    if (remainder == 0) return addr;
    return addr + (align - remainder);
}


void add_to_cache_list(kmem_cache_t* new_cache) { //function is not done yet
	kmem_state_t* my_kmem = curr_kmem();
    if (!new_cache) {
        kmem_cache_error(new_cache); // it will be that null cache error
        return;
    }

	acquire(&my_kmem->cache_list_lock);

    if (my_kmem->head_of_cache_list == NULL) {
        //list empty
        my_kmem->head_of_cache_list = new_cache;
        my_kmem->tail_of_cache_list = new_cache;
    }

    else {
		my_kmem->head_of_cache_list->prev = new_cache;
        new_cache->next = my_kmem->head_of_cache_list;
        my_kmem->head_of_cache_list = new_cache;
    }

    release(&my_kmem->cache_list_lock);

}
kmem_cache_t* remove_from_cache_list(kmem_cache_t* cachep)
{
    kmem_state_t* my_kmem = curr_kmem();

    acquire(&my_kmem->cache_list_lock);

    // pop-head behavior if cachep == NULL
    if (cachep == NULL) {
        kmem_cache_t *n = my_kmem->head_of_cache_list;
        if (!n) {
            release(&my_kmem->cache_list_lock);
            return NULL;
        }

        kmem_cache_t *next = n->next;

        my_kmem->head_of_cache_list = next;
        if (next) next->prev = NULL;
        else my_kmem->tail_of_cache_list = NULL; // list became empty

        n->next = NULL;
        n->prev = NULL;

        release(&my_kmem->cache_list_lock);
        return n;
    }

    // remove specific cachep
    // Optional: verify it's actually in the list (safe but O(n))
    // If you trust callers, you can skip the search and just unlink.
    kmem_cache_t *cur = my_kmem->head_of_cache_list;
    while (cur && cur != cachep) cur = cur->next;

    if (!cur) {
        // not found
        release(&my_kmem->cache_list_lock);
        return NULL;
    }

    kmem_cache_t *prev = cur->prev;
    kmem_cache_t *next = cur->next;

    if (prev) prev->next = next;
    else my_kmem->head_of_cache_list = next;   // removing head

    if (next) next->prev = prev;
    else my_kmem->tail_of_cache_list = prev;   // removing tail

    cur->next = NULL;
    cur->prev = NULL;

    release(&my_kmem->cache_list_lock);
    return cur;
}

static inline slab_t* remove_from_slab_list(slab_t **headp) {
    slab_t *s = *headp;
    if (!s) return NULL;

    slab_t *next = s->next;
    *headp = next;

    if (next) next->prev = NULL;

    s->next = NULL;
    s->prev = NULL;
    return s;
}

void my_kinit() {


    void* heap_start = (void*)end;
    int block_num = (PHYSTOP - (uint64)end) / BLOCK_SIZE;

    kmem_init(heap_start, block_num);
}





void kmem_init(void* space, int block_num)
{
    kmem_state_t* my_kmem = curr_kmem();

    // Initial raw region
    my_kmem->managed_start = (uint64)space;
    my_kmem->managed_num_blocks = block_num;

    uint64 region_end =
        my_kmem->managed_start + (uint64)block_num * PGSIZE;

    uint64 boot = my_kmem->managed_start;

    // ---- 1) free_area lives inside region ----
    my_kmem->free_area = (void**)boot;
    boot += (MAX_ORDER + 1) * sizeof(void*);

    // ---- 2) block_meta lives inside region ----
    my_kmem->block_meta = (block_meta_t*)boot;
    boot += (uint64)block_num * sizeof(block_meta_t);

    // ---- 3) cache descriptors live inside region ----
    my_kmem->cache_pool = (kmem_cache_t*)boot;
    boot += (uint64)MAX_NUM_CACHES * sizeof(kmem_cache_t);

    memset(my_kmem->free_area, 0,
           (MAX_ORDER+1)*sizeof(void*));
    memset(my_kmem->block_meta, 0,
           (uint64)block_num*sizeof(block_meta_t));
    memset(my_kmem->cache_pool, 0,
           (uint64)MAX_NUM_CACHES*sizeof(kmem_cache_t));

    // ---- Align boot to page boundary ----
    if(boot % PGSIZE)
        boot += (PGSIZE - (boot % PGSIZE));

    uint64 alloc_lo = boot;
    uint64 alloc_hi = region_end;

    uint64 avail = alloc_hi - alloc_lo;

    // ---- Protect minimum kernel heap size ----
    #define MIN_KERNEL_HEAP_BYTES (32ULL * 1024 * 1024)

    if(my_kmem == &kmem) {

        if(avail <= MIN_KERNEL_HEAP_BYTES)
            panic("not enough memory for kernel heap");

        uint64 win_avail = avail - MIN_KERNEL_HEAP_BYTES;

        int nwin =
            (int)(win_avail / (uint64)KHEAPWIN_SIZE);

        if(nwin < 1)
            panic("not enough space for windows");

        kheapwin_total =
            (uint64)nwin * (uint64)KHEAPWIN_SIZE;

        kheapwin_base0 =
            alloc_hi - kheapwin_total;

        kheapwin_base0 -=
            (kheapwin_base0 % PGSIZE);

        if(kheapwin_base0 <= alloc_lo)
            panic("window carve-out corrupted heap");

        // shrink kernel heap region
        alloc_hi = kheapwin_base0;
    }

    // ---- Final managed region ----
    my_kmem->managed_start = alloc_lo;
    my_kmem->managed_num_blocks =
        (int)((alloc_hi - alloc_lo) / PGSIZE);

    // ---- Initialize locks ----
    initlock(&my_kmem->cache_list_lock, "cache_list");
    initlock(&my_kmem->cache_pool_lock, "cache_pool");

    // ---- Initialize buddy allocator ----
    buddy_init();

    // ---- Create kmalloc caches ----
    my_kmem->kmalloc_caches[0]  = kmem_cache_create("size-32",    32,    NULL, NULL);
    my_kmem->kmalloc_caches[1]  = kmem_cache_create("size-64",    64,    NULL, NULL);
    my_kmem->kmalloc_caches[2]  = kmem_cache_create("size-128",   128,   NULL, NULL);
    my_kmem->kmalloc_caches[3]  = kmem_cache_create("size-256",   256,   NULL, NULL);
    my_kmem->kmalloc_caches[4]  = kmem_cache_create("size-512",   512,   NULL, NULL);
    my_kmem->kmalloc_caches[5]  = kmem_cache_create("size-1024",  1024,  NULL, NULL);
    my_kmem->kmalloc_caches[6]  = kmem_cache_create("size-2048",  2048,  NULL, NULL);
    my_kmem->kmalloc_caches[7]  = kmem_cache_create("size-4096",  4096,  NULL, NULL);
    my_kmem->kmalloc_caches[8]  = kmem_cache_create("size-8192",  8192,  NULL, NULL);
    my_kmem->kmalloc_caches[9]  = kmem_cache_create("size-16384", 16384, NULL, NULL);
    my_kmem->kmalloc_caches[10] = kmem_cache_create("size-32768", 32768, NULL, NULL);
    my_kmem->kmalloc_caches[11] = kmem_cache_create("size-65536", 65536, NULL, NULL);
    my_kmem->kmalloc_caches[12] = kmem_cache_create("size-131072",131072,NULL, NULL);
}


static inline int find_free_cache_slot() {
  kmem_state_t* my_kmem = curr_kmem();
  for(int i = 0; i < MAX_NUM_CACHES; i++){
    if(my_kmem->cache_pool[i].is_used == 0) return i;
  }
  return -1;
}

static inline uint16* slab_next_array(slab_t* s){
    return (uint16*)((char*)s + ALIGN_UP(sizeof(slab_t), sizeof(uint16)));
}

static inline void* slab_obj_ptr(kmem_cache_t* c, slab_t* s, uint16 idx){
    return (void*)((char*)s + c->obj_offset + (size_t)idx * c->object_size);
}

static inline uint16 slab_obj_index(kmem_cache_t* c, slab_t* s, void* obj){
    return (uint16)(((char*)obj - ((char*)s + c->obj_offset)) / c->object_size);
}


kmem_cache_t *kmem_cache_create(const char *name, size_t size,
                                void (*ctor)(void *), void (*dtor)(void *))
{
	kmem_state_t* my_kmem = curr_kmem();

    acquire(&my_kmem->cache_pool_lock);
	int index = find_free_cache_slot();
	if(index < 0) { release(&my_kmem->cache_pool_lock); return NULL; }
    kmem_cache_t *c = &my_kmem->cache_pool[index];
    c->is_used = 1;   // claim it while still holding lock
	c->destroying = 0;
    release(&my_kmem->cache_pool_lock);


    c->name = (char*)name;
    c->ctor = ctor;
    c->dtor = dtor;
    c->error_code = KMEM_OK;



    initlock(&c->lock, (char*) name);


    size_t obj = size;
    if (obj < sizeof(void*)) obj = sizeof(void*);
    obj = ALIGN_UP((uintptr_t) obj,(uintptr_t) sizeof(void*));
    c->object_size = obj;


    int min_objs = (obj > 2048) ? 1 : MIN_OBJS_PER_SLAB;

	c->slab_order = 0;
	while (c->slab_order <= MAX_SLAB_ORDER) {
  		size_t slab_bytes = (size_t)BLOCK_SIZE << c->slab_order;

  		// first assume header is just slab_t + next array (unknown yet), so solve by iteration:
  		// easiest: compute n ignoring header, then compute header, then recompute n once.

  		size_t usable0 = slab_bytes - ALIGN_UP(sizeof(slab_t), sizeof(void*));
  		size_t n0 = usable0 / obj;
  		if (n0 < (size_t)min_objs) { c->slab_order++; continue; }

  		// now compute real header including next[n0]
  		size_t header = ALIGN_UP(sizeof(slab_t) + n0 * sizeof(uint16), sizeof(void*));
  		size_t usable = (slab_bytes > header) ? (slab_bytes - header) : 0;
  		size_t n = usable / obj;

  		if (n >= (size_t)min_objs) {
    		c->objs_per_slab = n;
    		c->obj_offset = ALIGN_UP(sizeof(slab_t) + n * sizeof(uint16), sizeof(void*));
    		break;
  		}
	    c->slab_order++;
	}
	if (c->slab_order > MAX_SLAB_ORDER) {
		acquire(&my_kmem->cache_pool_lock);
  		c->is_used = 0;
  		release(&my_kmem->cache_pool_lock);
		return NULL;
	}

	c->next = c->prev = NULL;
	c->full = c->partial = c->empty = NULL;
	add_to_cache_list(c);
	return c;

}


int kmem_cache_shrink(kmem_cache_t *cachep) {
	kmem_state_t* my_kmem = curr_kmem();

    int freed = 0;

    for (;;) {
        acquire(&cachep->lock);
        slab_t *slab = remove_from_slab_list(&cachep->empty);
        release(&cachep->lock);

        if (!slab) break;

        // dtors etc (optional) - no shared state if objects aren't in use

        // clear owner_slab under buddy.lock
        acquire(&my_kmem->buddy_lock);
        int idx = addr_to_index(slab);
        int pages = 1 << cachep->slab_order;
        for (int i = 0; i < pages; i++) {
            my_kmem->block_meta[idx + i].owner_slab = NULL;
        }
        release(&my_kmem->buddy_lock);

        buddy_free((void*)slab); // buddy_free holds buddy.lock itself
        freed++;
    }

    return freed;
}

static inline void slab_list_push_front(slab_t **headp, slab_t *s)
{
    s->prev = NULL;
    s->next = *headp;
    if (*headp) (*headp)->prev = s;
    *headp = s;
}


static inline slab_t *slab_list_pop_front(slab_t **headp)
{
    slab_t *s = *headp;
    if (!s) return NULL;
    *headp = s->next;
    if (*headp) (*headp)->prev = NULL;
    s->next = s->prev = NULL;
    return s;
}

static inline void slab_list_remove_node(slab_t **headp, slab_t *s)
{
    if (s->prev) s->prev->next = s->next;
    else *headp = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}


static inline void* slab_alloc_obj(kmem_cache_t* c, slab_t* s){
    if(s->first_free == NOFREE) return NULL;
    uint16 idx = s->first_free;
    uint16 *next = slab_next_array(s);
    s->first_free = next[idx];
    s->free_count--;
    s->inuse++;
    return slab_obj_ptr(c, s, idx);
}

static inline void* page_align_down(const void *p) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    a &= ~(uintptr_t)(4096 - 1);
    return (void*)a;
}

static inline int get_num_slabs(slab_t* head) {
    int num_slabs = 0;
    slab_t* temp = head;
    while (temp) {
        temp = temp->next;
        num_slabs++;
    }

    return num_slabs;
}

static inline int get_objects_in_use(kmem_cache_t *cachep) {
    int in_use = 0;
    in_use += get_num_slabs(cachep->full) * cachep->objs_per_slab;

    slab_t* temp = cachep->partial;
    while (temp) {
        in_use += temp->inuse;
        temp = temp->next;
    }

    return in_use;
}




void *kmem_cache_alloc(kmem_cache_t *cachep)
{
	kmem_state_t* my_kmem = curr_kmem();

    if (!cachep) return NULL;

    // fast path: take from existing partial/empty slab
    acquire(&cachep->lock);
    if (cachep->destroying) { release(&cachep->lock); return NULL; }

    slab_t *slab = NULL;

    if (cachep->partial) {
        slab = cachep->partial;
    } else if (cachep->empty) {
        slab = slab_list_pop_front(&cachep->empty);
        slab_list_push_front(&cachep->partial, slab);
    }

    if (slab) {
        // IMPORTANT: slab_alloc_obj() must:
        //  - use the slab's index freelist (first_free/next/free_count)
        //  - increment slab->inuse exactly once
        void *obj = slab_alloc_obj(cachep, slab);
        if (!obj) {
            cachep->error_code = KMEM_ERR_UNINITIALIZED_OBJECT;
            release(&cachep->lock);
            return NULL;
        }

        // move slab between lists based on inuse AFTER allocation
        if (slab->inuse == cachep->objs_per_slab) {
            slab_list_remove_node(&cachep->partial, slab);
            slab_list_push_front(&cachep->full, slab);
        }

        release(&cachep->lock);
        return obj;
    }

    // need to grow: release cache lock before buddy
    release(&cachep->lock);

    void *base = buddy_alloc((uint64)(1 << cachep->slab_order));
    if (!base) {
        // (optional) you can set error_code under lock; not required for correctness
        acquire(&cachep->lock);
        cachep->error_code = KMEM_ERR_BUDDY_OOM;
        release(&cachep->lock);
        return NULL;
    }

    slab_t *newslab = (slab_t*)base;
    newslab->cache = cachep;
    newslab->inuse = 0;
    newslab->next = newslab->prev = NULL;

    // init index-based freelist in slab header
    uint16 *next = slab_next_array(newslab);   // must point to array inside slab header area
    for (uint16 i = 0; i + 1 < (uint16)cachep->objs_per_slab; i++)
        next[i] = (uint16)(i + 1);
    next[(uint16)cachep->objs_per_slab - 1] = NOFREE;

    newslab->first_free = 0;
    newslab->free_count = (uint16)cachep->objs_per_slab;

    // publish owner_slab for every page of this slab under buddy lock
    acquire(&my_kmem->buddy_lock);
    int head = addr_to_index(base);
    for (int i = 0; i < (1 << cachep->slab_order); i++) {
        my_kmem->block_meta[head + i].owner_slab = newslab;
    }
    release(&my_kmem->buddy_lock);

    // ctor once per object (NO pointer freelist writes into objects!)
    if (cachep->ctor) {
        for (uint16 i = 0; i < (uint16)cachep->objs_per_slab; i++) {
            void *o = slab_obj_ptr(cachep, newslab, i);
            cachep->ctor(o);
        }
    }

    // now attach slab to cache and allocate 1 object from it
    acquire(&cachep->lock);
    if (cachep->destroying) {
        // if being destroyed concurrently, don't publish it; free it back
        release(&cachep->lock);

        acquire(&my_kmem->buddy_lock);
        for (int i = 0; i < (1 << cachep->slab_order); i++) {
            my_kmem->block_meta[head + i].owner_slab = NULL;
        }
        release(&my_kmem->buddy_lock);

        buddy_free(base);
        return NULL;
    }

    slab_list_push_front(&cachep->partial, newslab);

    void *obj = slab_alloc_obj(cachep, newslab);   // <-- MUST be newslab
    if (!obj) {
        cachep->error_code = KMEM_ERR_UNINITIALIZED_OBJECT;
        // (optional) you could unlink and free the slab here, but usually slab_alloc_obj can't fail now
        release(&cachep->lock);
        return NULL;
    }

    if (newslab->inuse == cachep->objs_per_slab) {
        slab_list_remove_node(&cachep->partial, newslab);
        slab_list_push_front(&cachep->full, newslab);
    }

    release(&cachep->lock);
    return obj;
}




static inline void slab_free_obj(kmem_cache_t* c, slab_t* s, void* obj){
    uint16 idx = slab_obj_index(c, s, obj);
    uint16* next = slab_next_array(s);
    next[idx] = s->first_free;
    s->first_free = idx;
    s->free_count++;
    s->inuse--;
}










void kmem_cache_free(kmem_cache_t *cachep, void *objp) {
	kmem_state_t* my_kmem = curr_kmem();

    if (!cachep || !objp) return;



    void *page = page_align_down(objp);

    // 1) Find slab under buddy.lock (only for reading owner_slab)
    slab_t *slab = NULL;
    acquire(&my_kmem->buddy_lock);
    int idx = addr_to_index(page);
    slab = (idx >= 0) ? my_kmem->block_meta[idx].owner_slab : NULL;
    release(&my_kmem->buddy_lock);

    if (!slab) {
        // optional: set error under cache lock or accept benign race
        acquire(&cachep->lock);
        cachep->error_code = KMEM_ERR_CACHE_MISMATCH;
        release(&cachep->lock);
        return;
    }

    // 2) Now operate on slab and cache lists under cache lock only
    acquire(&cachep->lock);

    if (cachep->destroying) { release(&cachep->lock); return; }

    if (slab->cache != cachep) {
        cachep->error_code = KMEM_ERR_CACHE_MISMATCH;
        release(&cachep->lock);
        return;
    }

    if (slab->inuse == 0) {
        cachep->error_code = KMEM_ERR_DOUBLE_FREE;
        release(&cachep->lock);
        return;
    }

    int was_full = (slab->inuse == cachep->objs_per_slab);

    slab_free_obj(cachep, slab, objp);

    if (was_full) {
        slab_list_remove_node(&cachep->full, slab);
        slab_list_push_front(&cachep->partial, slab);
    }
    if (slab->inuse == 0) {
        slab_list_remove_node(&cachep->partial, slab);
        slab_list_push_front(&cachep->empty, slab);
    }

    release(&cachep->lock);
}


static inline int kmalloc_index(size_t sz){
  if(sz <= 32) return 0;
  if(sz <= 64) return 1;
  if(sz <= 128) return 2;
  if(sz <= 256) return 3;
  if(sz <= 512) return 4;
  if(sz <= 1024) return 5;
  if(sz <= 2048) return 6;
  if(sz <= 4096) return 7;
  if(sz <= 8192) return 8;
  if(sz <= 16384) return 9;
  if(sz <= 32768) return 10;
  if(sz <= 65536) return 11;
  if(sz <= 131072) return 12;
  return -1;
}


void* kmalloc(size_t size) {

    if (size == 0) return NULL;

	kmem_state_t* my_kmem = curr_kmem();

    size = ALIGN_UP((uintptr_t) size, (uintptr_t) 8);

    int idx = kmalloc_index(size);
	if(idx < 0) return NULL;
	kmem_cache_t *cache = my_kmem->kmalloc_caches[idx];
	if(!cache) return NULL;
	return kmem_cache_alloc(cache);




}



void k_free(const void *objp) {
	kmem_state_t* my_kmem = curr_kmem();

    void *page = page_align_down(objp);
    int idx = addr_to_index(page);
    if (idx < 0) return;

    slab_t* slab;
    acquire(&my_kmem->buddy_lock);
    slab = my_kmem->block_meta[idx].owner_slab;
    release(&my_kmem->buddy_lock);



    if (slab) {
        kmem_cache_free(slab->cache, (void*)objp);
        return;
    }

    // not slab → must be buddy allocation, and must be page-aligned base
    if (page != objp) {
        // panic: freeing non-base pointer
        return;
    }

    buddy_free((void*)objp);
}





void kmem_cache_destroy(kmem_cache_t *cachep) {
    kmem_state_t* my_kmem = curr_kmem();

    //if cache is part of the kmalloc_caches, don't allow desctruction
    for (int i = 0; i < KMALLOC_NUM_CACHES; i++) {
        if (my_kmem->kmalloc_caches[i] == cachep) {
            cachep->error_code = KMEM_ERR_DESTROY_KMALLOC_CACHE;
            kmem_cache_error(cachep);
            return;
        }
    }


    // Deallocate cache

    acquire(&cachep->lock);
    cachep->destroying = 1;

    //if objects are allocated and used by the kernel, destroying the cache holding them will cause an error
    if (cachep->partial || cachep->full) {
        cachep->error_code = KMEM_ERR_DESTROY_NOT_EMPTY;
        kmem_cache_error(cachep);
        cachep->destroying = 0;
        release(&cachep->lock);
        return;
    }

    release(&cachep->lock);

    //free all the empty slabs
    kmem_cache_shrink(cachep);


    //next step to deallocate the cache descriptor itself
    remove_from_cache_list(cachep);

    acquire(&cachep->lock);

    //mark the fields as zero to avoid bugs forming

	acquire(&my_kmem->cache_pool_lock);

    cachep->is_used = 0;

	release(&my_kmem->cache_pool_lock);

    cachep->name = NULL;
    cachep->error_code = 0;
    cachep->object_size = 0;
    cachep->objs_per_slab = 0;
    cachep->slab_order = 0;
    cachep->partial = NULL;
    cachep->full = NULL;
    cachep->empty = NULL;
    cachep->ctor = NULL;
    cachep->dtor = NULL;

    release(&cachep->lock);
}


void kmem_cache_info(kmem_cache_t *cachep) {
    if (!cachep) return;
    acquire(&cachep->lock);

    int num_empty_slabs = get_num_slabs(cachep->empty);
    int num_partial_slabs = get_num_slabs(cachep->partial);
    int num_full_slabs = get_num_slabs(cachep->full);
    int in_use = get_objects_in_use(cachep);

    int object_size = (int)cachep->object_size;
    int slab_order = cachep->slab_order;
    int objects_per_slab = cachep->objs_per_slab;

    release(&cachep->lock);

    int slab_size = 4096 << slab_order;

    int total_slabs = num_empty_slabs + num_partial_slabs + num_full_slabs;
    int capacity_objects = objects_per_slab * total_slabs;


    int percentage = 0;
    if (capacity_objects > 0) percentage = (in_use * 100) / capacity_objects;

    printf("=== kmem_cache_info ===\n");
    printf("name: %s\n", (cachep->name ? cachep->name : "(null)"));
    printf("object_size: %d\n", object_size);
    printf("slab_order: %d\n", slab_order);
    printf("slab_size_bytes: %d\n", slab_size);
    printf("objects_per_slab: %d\n", objects_per_slab);

    printf("slabs: empty=%d partial=%d full=%d total=%d\n", num_empty_slabs, num_partial_slabs, num_full_slabs, total_slabs);

    printf("objects: in_use=%d capacity=%d utilization=%d%%\n", in_use, capacity_objects, percentage);
}



int kmem_cache_error(kmem_cache_t *cachep)
{
    if (!cachep) {
        printf("kmem_cache_error: cachep is NULL\n");
        return KMEM_ERR_NULL_CACHE;
    }

    switch (cachep->error_code) {
        case KMEM_OK:
            printf("%s: OK\n", cachep->name);
            break;
        case KMEM_ERR_BUDDY_OOM:
            printf("%s: out of memory (buddy_alloc failed)\n", cachep->name);
            break;
        case KMEM_ERR_INVALID_FREE_PTR:
            printf("%s: invalid free pointer\n", cachep->name);
            break;
        case KMEM_ERR_DESTROY_NOT_EMPTY:
            printf("%s: destroy called but cache not empty\n", cachep->name);
            break;
        default:
            printf("%s: unknown error (%d)\n", cachep->name, cachep->error_code);
            break;
    }

    return cachep->error_code;
}






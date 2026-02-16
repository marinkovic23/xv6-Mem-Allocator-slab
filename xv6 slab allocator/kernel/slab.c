#include "slab.h"




#include "defs.h"




extern char end[];

typedef struct slab_s {
    kmem_cache_t* cache; // owner cache
    void *free_list;            // head of free objects in this slab
    unsigned inuse;             // allocated objects count
    slab_t* next;
    slab_t* prev;
} slab_t;

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

static inline uintptr_t ALIGN_UP(uintptr_t addr, uintptr_t align) {
    uintptr_t remainder = addr % align;
    if (remainder == 0) return addr;
    return addr + (align - remainder);
}


void add_to_cache_list(kmem_state_t* ks, kmem_cache_t* new_cache) { //function is not done yet

    if (!new_cache) {
        kmem_cache_error(ks, new_cache); // it will be that null cache error
        return;
    }

	acquire(&ks->cache_list_lock);

    if (ks->head_of_cache_list == NULL) {
        //list empty
        ks->head_of_cache_list = new_cache;
        ks->tail_of_cache_list = new_cache;
    }

    else {
		ks->head_of_cache_list->prev = new_cache;
        new_cache->next = ks->head_of_cache_list;
        ks->head_of_cache_list = new_cache;
    }

    release(&ks->cache_list_lock);

}
kmem_cache_t* remove_from_cache_list(kmem_state_t* ks, kmem_cache_t* cachep)
{
    acquire(&ks->cache_list_lock);

    // pop-head behavior if cachep == NULL
    if (cachep == NULL) {
        kmem_cache_t *n = ks->head_of_cache_list;
        if (!n) {
            release(&ks->cache_list_lock);
            return NULL;
        }

        kmem_cache_t *next = n->next;

        ks->head_of_cache_list = next;
        if (next) next->prev = NULL;
        else      ks->tail_of_cache_list = NULL; // list became empty

        n->next = NULL;
        n->prev = NULL;

        release(&ks->cache_list_lock);
        return n;
    }

    // remove specific cachep
    // Optional: verify it's actually in the list (safe but O(n))
    // If you trust callers, you can skip the search and just unlink.
    kmem_cache_t *cur = ks->head_of_cache_list;
    while (cur && cur != cachep) cur = cur->next;

    if (!cur) {
        // not found
        release(&ks->cache_list_lock);
        return NULL;
    }

    kmem_cache_t *prev = cur->prev;
    kmem_cache_t *next = cur->next;

    if (prev) prev->next = next;
    else      ks->head_of_cache_list = next;   // removing head

    if (next) next->prev = prev;
    else      ks->tail_of_cache_list = prev;   // removing tail

    cur->next = NULL;
    cur->prev = NULL;

    release(&ks->cache_list_lock);
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
    //renamed from kinit, because of the name clash with the freelist allocator that is implemented already

    void* heap_start = (void*)end;
    int block_num = (PHYSTOP - (uint64)end) / BLOCK_SIZE;

    kmem_init(ks, heap_start, block_num);
}


void kmem_init(kmem_state_t* ks, void* space, int block_num) {
    ks->heap_base = space;
    ks->heap_blocks = block_num;

    //part that initializes metadata to be used by the buddy allocator

    //free area, lists used by the buddy allocator
    uintptr_t boot = (uintptr_t)ks->heap_base;
    ks->free_area = (void**) boot;
    boot += (MAX_ORDER + 1) * sizeof(void*);


    //memory reserved for block metadata, this is necessary for the buddy to work properly
    ks->block_meta  = (block_meta_t*) boot;
    boot += block_num * sizeof(block_meta_t);


    //allocating space for kmem_cache_s structures;
    ks->cache_pool = (kmem_cache_t*) boot;
    boot += MAX_NUM_CACHES * sizeof(kmem_cache_t);
    ks->head_of_cache_list = NULL;
    ks->tail_of_cache_list = NULL;


    //aligning boot up to a page boundary
    boot = ALIGN_UP((uintptr_t) boot, (uintptr_t) BLOCK_SIZE);
    ks->managed_start = boot; //this is the start of the memory that is used by the allocator

    ks->managed_num_blocks = (PHYSTOP - (uint64)ks->managed_start) / BLOCK_SIZE;

    initlock(&ks->cache_list_lock, "cache_list");

    initlock(&ks->cache_pool_lock, "cache_pool");


	buddy_init(ks);
    //creating caches for generic objects of certain sizes, no non trivial dtor or ctor
    ks->kmalloc_caches[0] = kmem_cache_create(ks, "size-32", (size_t) 32, NULL, NULL);
    ks->kmalloc_caches[1] = kmem_cache_create(ks, "size-64", (size_t) 64, NULL, NULL);
    ks->kmalloc_caches[2] = kmem_cache_create(ks, "size-128", (size_t) 128, NULL, NULL);
    ks->kmalloc_caches[3] = kmem_cache_create(ks, "size-256", (size_t) 256, NULL, NULL);
    ks->kmalloc_caches[4] = kmem_cache_create(ks, "size-512", (size_t) 512, NULL, NULL);
    ks->kmalloc_caches[5] = kmem_cache_create(ks, "size-1024", (size_t) 1024, NULL, NULL);
    ks->kmalloc_caches[6] = kmem_cache_create(ks, "size-2048", (size_t) 2048, NULL, NULL);
    ks->kmalloc_caches[7] = kmem_cache_create(ks, "size-4096", (size_t) 4096, NULL, NULL);
    ks->kmalloc_caches[8] = kmem_cache_create(ks, "size-8192", (size_t) 8192, NULL, NULL);
    ks->kmalloc_caches[9] = kmem_cache_create(ks, "size-16384", (size_t) 16384, NULL, NULL);
	ks->kmalloc_caches[10] = kmem_cache_create(ks, "size-32768", (size_t) 32768, NULL, NULL);
	ks->kmalloc_caches[11] = kmem_cache_create(ks, "size-65536", (size_t) 65536, NULL, NULL);
	ks->kmalloc_caches[12] = kmem_cache_create(ks, "size-131072", (size_t) 131072, NULL, NULL);



}


static inline int find_free_cache_slot(kmem_state_t *ks) {
  for(int i = 0; i < MAX_NUM_CACHES; i++){
    if(ks->cache_pool[i].is_used == 0) return i;
  }
  return -1;
}


kmem_cache_t *kmem_cache_create(kmem_state_t* ks, const char *name, size_t size,
                                void (*ctor)(void *), void (*dtor)(void *))
{
    acquire(&ks->cache_pool_lock);
	int index = find_free_cache_slot(ks);
	if(index < 0) { release(&ks->cache_pool_lock); return NULL; }
    kmem_cache_t *c = &ks->cache_pool[index];
    c->is_used = 1;   // claim it while still holding lock
	c->destroying = 0;
    release(&ks->cache_pool_lock);


    c->name = (char*)name;
    c->ctor = ctor;
    c->dtor = dtor;
    c->error_code = KMEM_OK;



    initlock(&c->lock, (char*) name);


    size_t obj = size;
    if (obj < sizeof(void*)) obj = sizeof(void*);
    obj = ALIGN_UP((uintptr_t) obj,(uintptr_t) sizeof(void*));
    c->object_size = obj;

    size_t header = ALIGN_UP((uintptr_t) sizeof(slab_t),(uintptr_t) sizeof(void*));
    c->obj_offset = header;

    int min_objs = (obj > 2048) ? 1 : MIN_OBJS_PER_SLAB;

    c->slab_order = 0;
    while (c->slab_order <= MAX_SLAB_ORDER) {
        size_t slab_bytes = (size_t)BLOCK_SIZE << c->slab_order;
        size_t usable = (slab_bytes > header) ? (slab_bytes - header) : 0;
        size_t n = usable / obj;
        if (n >= (size_t)min_objs) {
            c->objs_per_slab = n;
            break;
        }
        c->slab_order++;
    }
    if (c->slab_order > MAX_SLAB_ORDER) return NULL;

    c->full = c->partial = c->empty = NULL;

    add_to_cache_list(c);
    return c;
}


int kmem_cache_shrink(kmem_state_t* ks, kmem_cache_t *cachep) {
    int freed = 0;

    for (;;) {
        acquire(&cachep->lock);
        slab_t *slab = remove_from_slab_list(&cachep->empty);
        release(&cachep->lock);

        if (!slab) break;

        // dtors etc (optional) - no shared state if objects aren't in use

        // clear owner_slab under buddy.lock
        acquire(&ks->buddy.lock);
        int idx = addr_to_index(ks, slab);
        int pages = 1 << cachep->slab_order;
        for (int i = 0; i < pages; i++) {
            ks->block_meta[idx + i].owner_slab = NULL;
        }
        release(&ks->buddy.lock);

        buddy_free(ks, (void*)slab); // buddy_free holds buddy.lock itself
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


static inline void *obj_pop(slab_t *slab) {
    void *obj = slab->free_list;
    if (obj) slab->free_list = *(void**)obj;
    return obj;
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




void *kmem_cache_alloc(kmem_state_t* ks, kmem_cache_t *cachep) {
    if (!cachep) return NULL;

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
        void *obj = obj_pop(slab);
        if (!obj) { cachep->error_code = KMEM_ERR_UNINITIALIZED_OBJECT; release(&cachep->lock); return NULL; }

        slab->inuse++;
        if (slab->inuse == cachep->objs_per_slab) {
            slab_list_remove_node(&cachep->partial, slab);
            slab_list_push_front(&cachep->full, slab);
        }

        release(&cachep->lock);
        return obj;
    }

    // Need to grow: release cache lock before calling buddy
    release(&cachep->lock);

    void *base = buddy_alloc(ks, 1 << cachep->slab_order);
    if (!base) { cachep->error_code = KMEM_ERR_BUDDY_OOM; return NULL; }

    slab_t *newslab = (slab_t*)base;


    newslab->cache = cachep;
    newslab->inuse = 0;
    newslab->next = newslab->prev = NULL;

    // publish owner_slab under buddy.lock
    acquire(&buddy.lock);
    int head = addr_to_index(ks, base);
    for (int i = 0; i < (1 << cachep->slab_order); i++) {
        ks->block_meta[head + i].owner_slab = newslab;
    }
    release(&buddy.lock);

    // build freelist (no shared state, ok without locks)
    char *obj0 = (char*)base + cachep->obj_offset;
    for (size_t i = 0; i + 1 < cachep->objs_per_slab; i++) {
        void *o = obj0 + i * cachep->object_size;
        void *n = obj0 + (i + 1) * cachep->object_size;
        if (cachep->ctor) cachep->ctor(o);
        *(void**)o = n;
    }
    void *last = obj0 + (cachep->objs_per_slab - 1) * cachep->object_size;
    if (cachep->ctor) cachep->ctor(last);
    *(void**)last = NULL;
    newslab->free_list = obj0;

    // now insert into cache lists under cache lock
    acquire(&cachep->lock);
    slab_list_push_front(&cachep->partial, newslab);

    void *obj = obj_pop(newslab);
    newslab->inuse = 1;

    if (newslab->inuse == cachep->objs_per_slab) {
        slab_list_remove_node(&cachep->partial, newslab);
        slab_list_push_front(&cachep->full, newslab);
    }

    release(&cachep->lock);
    return obj;
}



//helper function, add and remove from free list
//while free, the objects are linked together in a singly linked list
static inline void add_to_free_list(slab_t* slab, void* obj) {
    *(void**) obj = slab->free_list;
    slab->free_list = obj;
}

static inline void* remove_from_free_list(slab_t* slab) {
    void* obj = slab->free_list;
    if (obj) slab->free_list = *(void**) obj;


    return obj;
}










void kmem_cache_free(kmem_state_t* ks, kmem_cache_t *cachep, void *objp) {
    if (!cachep || !objp) return;

    void *page = page_align_down(objp);

    // 1) Find slab under buddy.lock (only for reading owner_slab)
    slab_t *slab = NULL;
    acquire(&ks->buddy.lock);
    int idx = addr_to_index(ks, page);
    slab = (idx >= 0) ? ks->block_meta[idx].owner_slab : NULL;
    release(&ks->buddy.lock);

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

    slab->inuse--;
    add_to_free_list(slab, objp);

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


static inline int kmalloc_index(size_t sz)
{
    if (sz <= 8) return 0;
    if (sz <= 16) return 1;
    if (sz <= 32) return 2;
    if (sz <= 64) return 3;
    if (sz <= 128) return 4;
    if (sz <= 256) return 5;
    if (sz <= 512) return 6;
    if (sz <= 1024) return 7;
    if (sz <= 2048) return 8;
    if (sz <= 4096) return 9;
    return -1;
}


void* kmalloc(kmem_state_t* ks, size_t size) {

    if (size == 0) return NULL;

    size = ALIGN_UP((uintptr_t) size, (uintptr_t) 8);

    size_t temp = size;
    int index = 0;
    while (temp > 8) {
        index++;
        temp = temp >> 1;
    }

    kmem_cache_t* cache = ks->kmalloc_caches[index];

	if (temp > BLOCK_SIZE) return NULL;

    return kmem_cache_alloc(ks, cache);




}



void k_free(kmem_state_t* ks, const void *objp) {
    void *page = page_align_down(objp);
    int idx = addr_to_index(ks, page);
    if (idx < 0) return;

    slab_t* slab;
    acquire(&ks->buddy.lock);
    slab = ks->block_meta[idx].owner_slab;
    release(&ks->buddy.lock);



    if (slab) {
        kmem_cache_free(ks, slab->cache, (void*)objp);
        return;
    }

    // not slab → must be buddy allocation, and must be page-aligned base
    if (page != objp) {
        // panic: freeing non-base pointer
        return;
    }

    buddy_free(ks, (void*)objp);
}





void kmem_cache_destroy(kmem_state_t* ks, kmem_cache_t *cachep) {


    //if cache is part of the kmalloc_caches, don't allow desctruction
    for (int i = 0; i < KMALLOC_NUM_CACHES; i++) {
        if (ks->kmalloc_caches[i] == cachep) {
            cachep->error_code = KMEM_ERR_DESTROY_KMALLOC_CACHE;
            kmem_cache_error(ks, cachep);
            return;
        }
    }


    // Deallocate cache

    acquire(&cachep->lock);
    cachep->destroying = 1;

    //if objects are allocated and used by the kernel, destroying the cache holding them will cause an error
    if (cachep->partial || cachep->full) {
        cachep->error_code = KMEM_ERR_DESTROY_NOT_EMPTY;
        kmem_cache_error(ks, cachep);
        cachep->destroying = 0;
        release(&cachep->lock);
        return;
    }

    release(&cachep->lock);

    //free all the empty slabs
    kmem_cache_shrink(ks, cachep);


    //next step to deallocate the cache descriptor itself
    remove_from_cache_list(ks, cachep);

    acquire(&cachep->lock);

    //mark the fields as zero to avoid bugs forming

	acquire(&ks->cache_pool_lock);

    cachep->is_used = 0;

	release(&ks->cache_pool_lock);

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


void kmem_cache_info(kmem_state_t* ks, kmem_cache_t *cachep) {
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



int kmem_cache_error(kmem_state_t* ks, kmem_cache_t *cachep)
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






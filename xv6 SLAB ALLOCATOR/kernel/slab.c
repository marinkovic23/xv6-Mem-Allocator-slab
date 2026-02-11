#include "slab.h"

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
    KMEM_ERR_DESTROY_KMALLOC_CACHE
};


struct spinlock cache_list_lock;

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

    //spinlock_t spinlock; will be done at some point

    kmem_cache_t* next;
    kmem_cache_t* prev;
};

static kmem_cache_t *kmalloc_caches[11]; // 8..4096 (order 3..12)

void add_to_cache_list(kmem_cache_t* new_cache) { //function is not done yet

    if (!new_cache) {
        kmem_cache_error(new_cache); // it will be that null cache error
        return;
    }

	acquire(&cache_list_lock);

    if (head_of_cache_list == NULL) {
        //list empty
        head_of_cache_list = new_cache;
        tail_of_cache_list = new_cache;
    }

    else {
		head_of_cache_list->prev = new_cache;
        new_cache->next = head_of_cache_list;
        head_of_cache_list = new_cache;
    }

    release(&cache_list_lock);

}
kmem_cache_t* remove_from_cache_list() { //check this function
    if (head_of_cache_list == NULL) {
        //list empty, there is nothing to do here
        return NULL;
    }

	acquire(&cache_list_lock);

    else if (head_of_cache_list == tail_of_cache_list) {
        kmem_cache_t* temp;
        temp = head_of_cache_list; //deep or shallow copy and does it make a difference
        head_of_cache_list = NULL;
        tail_of_cache_list = NULL;

		release(&cache_list_lock);

        return temp;
    }

    else {
        kmem_cache_t* temp;
        temp = head_of_cache_list;
        head_of_cache_list->next->prev = NULL;
        head_of_cache_list = head_of_cache_list->next;
		temp->next = NULL;

		release(&cache_list_lock);
        return temp;
    }

    //possibly done need to test this
}



void my_kinit() {
    //renamed from kinit, because of the name clash with the freelist allocator that is implemented already

    void* heap_start = (void*)end;
    int block_num = (PHYSTOP - (uint64)end) / BLOCK_SIZE;

    kmem_init(heap_start, block_num);
}


void kmem_init(void* space, int block_num) {
    heap_base = space;
    heap_blocks = block_num;

    //part that initializes metadata to be used by the buddy allocator

    //free area, lists used by the buddy allocator
    uintptr_t boot = (uintptr_t)heap_base;
    free_area = (void**) boot;
    boot += (MAX_ORDER + 1) * sizeof(void*);


    //memory reserved for block metadata, this is necessary for the buddy to work properly
    block_meta  = (block_meta_t*) boot;
    boot += block_num * sizeof(block_meta_t);


    //allocating space for kmem_cache_s structures;
    cache_pool = (kmem_cache_t*) boot;
    boot += MAX_NUM_CACHES * sizeof(kmem_cache_t);
    head_of_cache_list = NULL;
    tail_of_cache_list = NULL;


    //aligning boot up to a page boundary
    boot = ALIGN_UP(boot, BLOCK_SIZE);
    managed_start = boot; //this is the start of the memory that is used by the allocator

    managed_num_blocks = (PHYSTOP - (uint64)managed_start) / BLOCK_SIZE; //these need to be global

	buddy_init();
    //creating caches for generic objects of certain sizes, no non trivial dtor or ctor
    kmem_caches[0] = kmem_cache_create("kmalloc_8", (size_t) 8, NULL, NULL);
    kmem_caches[1] = kmem_cache_create("kmalloc_16", (size_t) 16, NULL, NULL);
    kmem_caches[2] = kmem_cache_create("kmalloc_32", (size_t) 32, NULL, NULL);
    kmem_caches[3] = kmem_cache_create("kmalloc_64", (size_t) 64, NULL, NULL);
    kmem_caches[4] = kmem_cache_create("kmalloc_128", (size_t) 128, NULL, NULL);
    kmem_caches[5] = kmem_cache_create("kmalloc_256", (size_t) 256, NULL, NULL);
    kmem_caches[6] = kmem_cache_create("kmalloc_512", (size_t) 512, NULL, NULL);
    kmem_caches[7] = kmem_cache_create("kmalloc_1024", (size_t) 1024, NULL, NULL);
    kmem_caches[8] = kmem_cache_create("kmalloc_2048", (size_t) 2048, NULL, NULL);
    kmem_caches[9] = kmem_cache_create("kmalloc_4096", (size_t) 4096, NULL, NULL);



}

static inline uintptr_t ALIGN_UP(uintptr_t addr, uintptr_t align) {
    uintptr_t remainder = addr % align;
    if (remainder == 0) return addr;
    return addr + (align - remainder);
}

static inline int find_free_cache_slot(void) {
    for (int i = 0; i < MAX_NUM_CACHES; i++) {
		if (cache_pool[i].is_used == 0) return i;
	}
    return -1;
}


kmem_cache_t *kmem_cache_create(const char *name, size_t size,
                                void (*ctor)(void *), void (*dtor)(void *))
{
    int index = find_free_cache_slot();
    if (index < 0) return NULL;

    kmem_cache_t *c = &cache_pool[index];

    c->is_used = 1;
    c->name = (char*)name;
    c->ctor = ctor;
    c->dtor = dtor;
    c->error_code = KMEM_OK;


    size_t obj = size;
    if (obj < sizeof(void*)) obj = sizeof(void*);
    obj = ALIGN_UP(obj, sizeof(void*));
    c->object_size = obj;

    size_t header = ALIGN_UP(sizeof(slab_t), sizeof(void*));
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


int kmem_cache_shrink(kmem_cache_t *cachep) {
    int freed = 0;

    while (cachep->empty) {
        slab_t *slab = remove_from_slab_list(&cachep->empty);

        char *base = (char*)slab;


        char *obj0 = (char*)slab + cachep->obj_offset;



        for (int i = 0; i < cachep->objs_per_slab; i++) {
            void *obj = obj0 + (size_t)i * cachep->object_size;
            if (cachep->dtor) cachep->dtor(obj);
        }


        // clear page->slab ownership mapping if you use it
        void* first_byte = (void*) slab;
        int idx = addr_to_index(first_byte);



        int order = cachep->slab_order;

        for (int i = 0; i < 1 << order; i++) {
            block_meta[idx + i].owner_slab = NULL;
        }

        // clear_owned_by_slab(slab, cachep->slab_order);

        buddy_free((void*)slab);
        freed++;
    }
    return freed;
}




static inline void *obj_pop(slab_t *slab) {
    void *obj = slab->free_list;
    if (obj) slab->free_list = *(void**)obj;
    return obj;
}

void *kmem_cache_alloc(kmem_cache_t *cachep) {
    if (!cachep) return NULL;

    slab_t *slab;

    if (cachep->partial) {
        slab = cachep->partial;
    } else if (cachep->empty) {
        slab = slab_list_pop_front(&cachep->empty);
        slab_list_push_front(&cachep->partial, slab);
    } else {
        void *base = buddy_alloc(1 << cachep->slab_order);
        if (!base) {
            cachep->error_code = KMEM_ERR_BUDDY_OOM;
            return NULL;
        }

		slab = (slab_t*)base;

		int head = addr_to_index(base);
		for (int i = 0; i < (1 << cachep->slab_order); i++) {
    		block_meta[head + i].owner_slab = slab;
		}

        slab = (slab_t*)base;
        slab->cache = cachep;
        slab->inuse = 0;
        slab->next = slab->prev = NULL;

        // IMPORTANT: use obj_offset
        char *obj0 = (char*)base + cachep->obj_offset;

        // build freelist
        for (size_t i = 0; i + 1 < cachep->objs_per_slab; i++) {
            void *o = obj0 + i * cachep->object_size;
            void *n = obj0 + (i + 1) * cachep->object_size;
            if (cachep->ctor) cachep->ctor(o);
            *(void**)o = n;
        }
        void *last = obj0 + (cachep->objs_per_slab - 1) * cachep->object_size;
        if (cachep->ctor) cachep->ctor(last);
        *(void**)last = NULL;

        slab->free_list = obj0;

        slab_list_push_front(&cachep->partial, slab);

        // also: set block_meta[idx+i].owner_slab = slab for all pages in this slab
    }

    void *obj = obj_pop(slab);
    if (!obj) {
        cachep->error_code = KMEM_ERR_INTERNAL; // define this or reuse something
        return NULL;
    }

    slab->inuse++;

    if (slab->inuse == cachep->objs_per_slab) {
        slab_list_remove_node(&cachep->partial, slab);
        slab_list_push_front(&cachep->full, slab);
    }

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






void kmem_cache_free(kmem_cache_t *cachep, void *objp)
{
    if (!cachep || !objp) return;

    // find slab via page->slab mapping
    void *page = page_align_down(objp);
    int idx = addr_to_index(page);
    slab_t *slab = block_meta[idx].owner_slab;

    if (!slab || slab->cache != cachep) {
        cachep->error_code = KMEM_ERR_CACHE_MISMATCH;
        return;
    }

    int was_full = (slab->inuse == cachep->objs_per_slab);

 	//decrementing
    if (slab->inuse == 0) {
        cachep->error_code = KMEM_ERR_DOUBLE_FREE; // or internal
        return;
    }
    slab->inuse--;

    //add obj to freelist
    add_to_free_list(slab, objp);

    // move between lists
    if (was_full) {
        slab_list_remove_node(&cachep->full, slab);
        slab_list_push_front(&cachep->partial, slab);
    }

    if (slab->inuse == 0) {
        // it is currently in partial (unless objs_per_slab==0, impossible)
        slab_list_remove_node(&cachep->partial, slab);
        slab_list_push_front(&cachep->empty, slab);
    }
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


void* kmalloc(size_t size) {

    if (size == 0) return NULL;

    size = ALIGN_UP(size, 8);

    size_t temp = size
    int index = 0;
    while (temp > 8) {
        index++;
        temp = temp >> 1;
    }

    cache = kmalloc_caches[index];

	if (temp > BLOCK_SIZE) return NULL;

    return kmem_cache_alloc(cache);




}



void kfree(const void *objp) {
    void *page = page_align_down(objp);
    int idx = addr_to_index(page);
    if (idx < 0) return;

    slab_t *slab = block_meta[idx].owner_slab;

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



void kmem_cache_destroy(kmem_cache_t *cachep) {


    //if cache is part of the kmalloc_caches, don't allow desctruction
    for (int i = 0; i < KMALLOC_NUM_CACHES; i++) {
        if (kmalloc_caches[i] == cachep) {
            cachep->error_code = KMEM_ERR_DESTROY_KMALLOC_CACHE;
            kmem_cache_error(cachep);
            return;
        }
    }


    // Deallocate cache

    //if objects are allocated and used by the kernel, destroying the cache holding them will cause an error
    if (cachep->partial || cachep->full) {
        cachep->error_code = KMEM_ERR_DESTROY_NOT_EMPTY;
        kmem_cache_error(cachep);
        return;
    }


    //free all the empty slabs
    int num_freed_slabs = kmem_cache_shrink(cachep);


    //next step to deallocate the cache descriptor itself
    remove_from_cache_list(cachep);

    //mark the fields as zero to avoid bugs forming
    cachep->is_used = 0;
    cachep->name = NULL;
    cachep->error_code = 0;
    cachep->object_size = 0;
    cachep->objects_per_slab = 0;
    cachep->slab_order = 0;
    cachep->partial = NULL;
    cachep->full = NULL;
    cachep->empty = NULL;
    cachep->ctor = NULL;
    cachep->dtor = NULL;
}


void kmem_cache_info(kmem_cache_t *cachep) {
    int num_empty_slabs = get_num_slabs(cachep->empty);
    int num_partial_slabs = get_num_slabs(cachep->partial);
    int num_full_slabs = get_num_slabs(cachep->full);

    int object_size = (int)cachep->object_size;
    int slab_order = cachep->slab_order;
    int objects_per_slab = cachep->objects_per_slab;

    int slab_size = 4096 << slab_order;

    int total_slabs = num_empty_slabs + num_partial_slabs + num_full_slabs;
    int capacity_objects = objects_per_slab * total_slabs;

    int in_use = get_objects_in_use(cachep);

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





static inline void* page_align_down(const void *p) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    a &= ~(uintptr_t)(4096 - 1);
    return (void*)a;
}

static inline int get_num_slabs(slab_t* head) {
    int num_slabs = 0;
    temp = head;
    while (temp) {
        temp = temp->next;
        num_slabs++;
    }

    return num_slabs;
}

static inline int get_objects_in_use(kmem_cache_t *cachep) {
    int in_use = 0;
    in_use += get_num_slabs(cachep->full) * cachep->objects_per_slab;

    slab_t* temp = cachep->partial;
    while (temp) {
        in_use += temp->inuse;
        temp = temp->next;
    }

    return in_use;
}
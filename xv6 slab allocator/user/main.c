#include "kernel/types.h"
#include "kernel/stat.h"
#include "user.h"
#include "kernel/param.h"
#include "ucache.h"

#define RUN_NUM (5)
#define ITERATIONS (1000)

#define shared_size (7)
#define MASK (0xA5)
#define BLOCK_SIZE (4096)

typedef unsigned long size_t;

struct data_s {
    int id;
    kmem_cache_t  *shared;
    int iterations;
};

const char * const CACHE_NAMES[] = {"tc_0",
                                    "tc_1",
                                    "tc_2",
                                    "tc_3",
                                    "tc_4"};

void ct(void *data) {
    static int i = 1;
    printf("%d Shared object constructed.\n", i++);
    memset(data, MASK, shared_size);
}

void construct(void *data) {
	ct(data);
}

int check(void *data, size_t size) {
    int ret = 1;
    for (int i = 0; i < size; i++) {
        if (((unsigned char *)data)[i] != MASK) {
            ret = 0;
        }
    }

    return ret;
}

struct objects_s {
    kmem_cache_t *cache;
    void *data;
};


void work(void* pdata) {
    struct data_s data = *(struct data_s*) pdata;
    int size = 0;
    int object_size = data.id + 1;
    kmem_cache_t *cache = kmem_cache_create(CACHE_NAMES[data.id], object_size, 0, 0);

    struct objects_s *objs = (struct objects_s*)(kmalloc(sizeof(struct objects_s) * data.iterations));

    for (int i = 0; i < data.iterations; i++) {
        if (i % 100 == 0) {
            objs[size].data = kmem_cache_alloc(data.shared);
            objs[size].cache = data.shared;
            if (!check(objs[size].data, shared_size)) {
                printf("Value not correct!");
            }
        }
        else {
            objs[size].data = kmem_cache_alloc(cache);
            objs[size].cache = cache;
            memset(objs[size].data, MASK, object_size);
        }
        size++;
    }


    kmem_cache_info(cache); //added for debugging


    kmem_cache_info(data.shared);

    for (int i = 0; i < size; i++) {

        if (!check(objs[i].data, (cache == objs[i].cache) ? object_size : shared_size)) { //old line get this back later



            printf("Value not correct!");
        }
        kmem_cache_free(objs[i].cache, objs[i].data);
    }

    kfree(objs);
    kmem_cache_destroy(cache);
}



void runs(void(*work)(void*), struct data_s* data, int num) {
    for (int i = 0; i < num; i++) {
        struct data_s private_data;
        private_data = *(struct data_s*) data;
        private_data.id = i;
        work(&private_data);
    }
}

int main(void) {
    int num_of_blocks = 1024;
    void* space = malloc(num_of_blocks * BLOCK_SIZE);
	printf("space=%p\n", space);
    kmem_init(space, num_of_blocks);
	//printf("returned from kmem_init\n");
	printf("construct addr=%lx\n", (uint64)construct);
    kmem_cache_t *shared = kmem_cache_create("shared object", shared_size, construct, 0);

    struct data_s data;
    data.shared = shared;
    data.iterations = ITERATIONS;
    runs(work, &data, RUN_NUM);

    kmem_cache_destroy(shared);
    free(space);

	exit(0);
}

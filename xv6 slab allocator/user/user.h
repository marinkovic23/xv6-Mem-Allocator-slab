#include "kernel/types.h"

#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sys_sbrk(int,int);
int pause(int);
int uptime(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
char* gets(char*, int max);
unsigned int strlen(const char*);
void* memset(void*, int, unsigned int); //wherever there is unsigned int, there used to be uint, be careful of that
int atoi(const char*);
int memcmp(const void *, const void *, unsigned int);
void *memcpy(void *, const void *, unsigned int);
char* sbrk(int);
char* sbrklazy(int);

// printf.c
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));

// umalloc.c
void* malloc(unsigned int);
void free(void*);




// added things
typedef struct kmem_cache_s kmem_cache_t;

uint64 k_kmem_cache_create(const char *name, int size, uint64 ctor, uint64 dtor);
uint64 k_kmem_cache_alloc(uint64 cache);
int k_kmem_cache_free(uint64 cache, uint64 obj);
int k_kmem_cache_destroy(uint64 cache);


void kmem_init(void *space, int block_num);
kmem_cache_t *kmem_cache_create(const char *name, int size, void (*ctor)(void*), void (*dtor)(void*));
void *kmem_cache_alloc(kmem_cache_t *cache);
uint64 kmem_cache_free(kmem_cache_t *cache, void *obj);
uint64 kmem_cache_destroy(kmem_cache_t *cache);
int kmem_cache_info(kmem_cache_t *cache);

void *kmalloc(int size);
int kfree(void *p);



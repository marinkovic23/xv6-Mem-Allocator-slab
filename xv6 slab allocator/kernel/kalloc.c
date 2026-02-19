// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.


#include "slab.h"
//#include "types.h"
//#include "param.h"
//#include "memlayout.h"
//#include "spinlock.h"
//#include "riscv.h"
#include "defs.h"



void
kinit()
{
}

void
kfree(void *pa)
{

  buddy_free(pa); //find the locks

  return;

}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{

  void* ptr = buddy_alloc(1); //just call buddy don't worry about it
  return ptr;
}

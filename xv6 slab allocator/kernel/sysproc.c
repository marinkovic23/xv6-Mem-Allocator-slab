#include "slab.h" //i can't include this safely
//#include "types.h"
//#include "riscv.h"
#include "defs.h"
//#include "param.h"
//#include "memlayout.h"
//#include "spinlock.h"
#include "proc.h"
#include "vm.h"




uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


uint64
sys_kmem_init(void)
{
  uint64 uva;
  int pages;

  argaddr(0, &uva);
  argint(1, &pages);

  if(pages <= 0) return -1;
  if(uva % PGSIZE) return -1;

  struct proc *p = myproc();

  // allocate per-process kmem_state if missing
  if(p->kmem_priv == 0){
    p->kmem_priv = (struct kmem_state*)kalloc();
    if(p->kmem_priv == 0) return -1;
    memset(p->kmem_priv, 0, PGSIZE);
  }

  // choose a per-process kernel window base (must not collide across processes)
  // simplest: reserve a fixed window per pid
  // define in memlayout.h:
  //   #define KHEAPWIN_BASE  0x...... (some free kernel VA range)
  //   #define KHEAPWIN_STRIDE (MAX_TEST_PAGES*PGSIZE)
  //   #define MAX_TEST_PAGES 2048 (or whatever you allow)
  if(pages > MAX_TEST_PAGES) return -1;

  uint64 kva_base = KHEAPWIN_BASE + (uint64)p->pid * KHEAPWIN_SIZE;
  p->kheapwin_base = kva_base;
  p->kheapwin_pages = pages;

  // map each user page to contiguous kernel VA
  for(int i = 0; i < pages; i++){
    uint64 uva_i = uva + (uint64)i * PGSIZE;

    uint64 pa = walkaddr(p->pagetable, uva_i);
    if(pa == 0){
      pa = vmfault(p->pagetable, uva_i, 0);
    }
    if(pa == 0) return -1;

    uint64 kva_i = kva_base + (uint64)i * PGSIZE;

    // map into kernel page table
    if(mappages(kernel_pagetable, kva_i, PGSIZE, pa, PTE_R|PTE_W) != 0)
      return -1;
  }

  sfence_vma();

  kmem_init(p->kmem_priv, (void*)kva_base, pages);
  return 0;
}

uint64
sys_kmem_cache_create(void)
{
  uint64 uname;
  int size;
  uint64 uctor, udtor;

  argaddr(0, &uname);
  argint(1, &size);
  argaddr(2, &uctor);
  argaddr(3, &udtor);

  if(size <= 0) return 0;

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return 0; // must call kmem_init first

  char kname[64];
  if(copyinstr(p->pagetable, kname, uname, sizeof(kname)) < 0)
    return 0;

  // make name persistent (do NOT store pointer to stack buffer)
  char *namecopy = (char*)kalloc();
  if(namecopy == 0) return 0;
  memset(namecopy, 0, PGSIZE);
  safestrcpy(namecopy, kname, PGSIZE);

  // ctor/dtor: for your test they are 0, so ignore them
  kmem_cache_t *c = kmem_cache_create(p->kmem_priv, namecopy, (size_t)size, 0, 0);
  return (uint64)c;
}

uint64
sys_kmem_cache_alloc(void)
{
  uint64 hc;
  argaddr(0, &hc);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return 0;

  kmem_cache_t *c = (kmem_cache_t*)hc;
  if(c == 0) return 0;

  void *obj = kmem_cache_alloc(p->kmem_priv, c);
  return (uint64)obj;
}

uint64
sys_kmem_cache_free(void)
{
  uint64 hc, obj;
  argaddr(0, &hc);
  argaddr(1, &obj);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return -1;

  kmem_cache_t *c = (kmem_cache_t*)hc;
  if(c == 0 || obj == 0) return -1;

  kmem_cache_free(p->kmem_priv, c, (void*)obj);
  return 0;
}

uint64
sys_kmem_cache_destroy(void)
{
  uint64 hc;
  argaddr(0, &hc);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return -1;

  kmem_cache_t *c = (kmem_cache_t*)hc;
  if(c == 0) return -1;

  kmem_cache_destroy(p->kmem_priv, c);
  return 0;
}

uint64
sys_kmem_cache_info(void)
{
  uint64 hc;
  argaddr(0, &hc);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return -1;

  kmem_cache_t *c = (kmem_cache_t*)hc;
  if(c == 0) return -1;

  kmem_cache_info(p->kmem_priv, c);
  return 0;
}

uint64
sys_kmalloc(void)
{
  int n;
  argint(0, &n);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return 0;
  if(n <= 0) return 0;

  void *obj = kmalloc(p->kmem_priv, (size_t)n);
  return (uint64)obj;
}

uint64
sys_kfree(void)
{
  uint64 up;
  argaddr(0, &up);

  struct proc *p = myproc();
  if(p->kmem_priv == 0) return -1;
  if(up == 0) return -1;

  k_free(p->kmem_priv, (void*)up);
  return 0;
}





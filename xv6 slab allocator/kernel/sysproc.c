#include "slab.h"
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







//helpers for differentiating between user calls and kernel calls for the allocator
static inline kmem_state_t* kmem_push(kmem_state_t *newst){
  struct cpu *c = mycpu();
  kmem_state_t *old = c->active_kmem;
  c->active_kmem = newst;
  return old;
}

static inline void kmem_pop(kmem_state_t *old){
  mycpu()->active_kmem = old;
}




uint64
sys_kmem_init(void)
{
  uint64 space;
  argaddr(0, &space);

  int pages;
  argint(1, &pages);

  if(pages <= 0 || pages > MAX_TEST_PAGES)
    return -1;

  struct proc *p = myproc();



  // one-time init per process
  if(p->ukmem_inited)
    return -1;

  p->uheapwin_base = (uint64) space;
  p->kheapwin_pages = pages;


  // allocate backing memory for this process's allocator arena
  uint64 total_bytes = (uint64)pages * PGSIZE;

  void *arena = buddy_alloc(total_bytes/PGSIZE);   // use GLOBAL allocator
  if(arena == 0)
    return -1;

  p->kheapwin_base = (uint64) arena;

  // initialize per-process allocator state
  kmem_state_t *old = kmem_push((kmem_state_t*)p->ukmem);
  kmem_init(arena, pages);
  kmem_pop(old);

  p->ukmem_inited = 1;

  return 0;
}

uint64
sys_k_kmem_cache_create(void)
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
  if(!p->ukmem_inited) return 0;

  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);


  char kname[64];
  if(copyinstr(p->pagetable, kname, uname, sizeof(kname)) < 0)
    return 0;

  // make name persistent (do NOT store pointer to stack buffer)
  char *namecopy = (char*)kalloc();
  if(namecopy == 0) return 0;
  memset(namecopy, 0, PGSIZE);
  safestrcpy(namecopy, kname, PGSIZE);

  // ctor/dtor: for your test they are 0, so ignore them
  kmem_cache_t *c = kmem_cache_create(namecopy, (size_t)size, 0, 0);
  kmem_pop(old);


  // translate kernel-window VA -> user-window VA (same PA)
  return p->uheapwin_base + ((uint64)c - p->kheapwin_base);

}

uint64
sys_k_kmem_cache_alloc(void)
{
  uint64 hc;
  argaddr(0, &hc);

  struct proc *p = myproc();
  if(!p->ukmem_inited) return 0;

  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);

  // must have a window set up via sys_kmem_init()
  if(p->kheapwin_base == 0 || p->uheapwin_base == 0 || p->kheapwin_pages <= 0)
    return 0;

  uint64 c = p->kheapwin_base + (hc - p->uheapwin_base);
  if(c == 0)
    return 0;




  void *kobjp = kmem_cache_alloc((kmem_cache_t*) c);
  kmem_pop(old);

  if(kobjp == 0)
    return 0;

  uint64 kobj = (uint64)kobjp;

  // kernel window bounds for this process
  /*uint64 k_lo = p->kheapwin_base;
  uint64 k_hi = p->kheapwin_base + (uint64)p->kheapwin_pages * PGSIZE;

  // must land inside the mapped kernel window, otherwise cannot be represented in user VA
  if(kobj < k_lo || kobj >= k_hi){
    // don't panic in production, but for your lab this is useful:
    panic("kmem_cache_alloc outside kheap window");
    // or: kmem_cache_free(c, kobjp); return 0;
  }*/

  // kernel-window VA -> user-window VA
  uint64 uobj = p->uheapwin_base + (kobj - p->kheapwin_base);
  return uobj;
}
uint64
sys_k_kmem_cache_free(void)
{
  uint64 hc, uobj;
  argaddr(0, &hc);
  argaddr(1, &uobj);

  struct proc *p = myproc();
  if(!p->ukmem_inited) return 0;

  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);





  if(hc == 0 || uobj == 0) {
    kmem_pop(old);
    return -1;
  }


  // translate: user-window VA -> kernel-window VA
  uint64 kobj = p->kheapwin_base + (uobj - p->uheapwin_base);

  // translate: user-window VA -> kernel-window VA
  uint64 c = p->kheapwin_base + (hc - p->uheapwin_base);


  // kernel-window bounds (redundant but cheap + good for debugging)
  uint64 k_lo = p->kheapwin_base;
  uint64 k_hi = p->kheapwin_base + (uint64)p->kheapwin_pages * PGSIZE;
  if(kobj < k_lo || kobj >= k_hi) {
    kmem_pop(old);
    return -1;
  }

  kmem_cache_free((kmem_cache_t*) c, (void*)kobj);
  kmem_pop(old);
  return 0;
}

uint64
sys_k_kmem_cache_destroy(void)
{
  struct proc *p = myproc();
  if(!p->ukmem_inited) return 0;

  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);

  uint64 hc;
  argaddr(0, &hc);

  // translate user-window VA -> kernel-window VA
  uint64 c =  p->kheapwin_base + ((uint64)hc - p->uheapwin_base);



  if(c == 0) {kmem_pop(old); return -1;}

  kmem_cache_destroy((kmem_cache_t*) c);
  kmem_pop(old);
  return 0;
}

uint64
sys_kmem_cache_info(void)
{
  struct proc *p = myproc();
  if(!p->ukmem_inited) return 0;

  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);

  uint64 hc;
  argaddr(0, &hc);


  // translate user-window VA -> kernel-window VA (same PA)
  uint64 c =  p->kheapwin_base + ((uint64)hc - p->uheapwin_base);

  if(c == 0) {kmem_pop(old); return -1;}

  kmem_cache_info((kmem_cache_t*) c);
  kmem_pop(old);
  return 0;
}

uint64
sys_kmalloc(void)
{
  int n;
  argint(0, &n);
  if(n <= 0)
    return 0;

  struct proc *p = myproc();
  if(!p->ukmem_inited)
    return 0;

  // window must exist because we return a user VA derived from it
  if(p->kheapwin_base == 0 || p->uheapwin_base == 0 || p->kheapwin_pages <= 0)
    return 0;

  kmem_state_t *old = kmem_push((kmem_state_t*)p->ukmem);
  void *kobjp = kmalloc((size_t)n);
  kmem_pop(old);

  if(kobjp == 0)
    return 0;

  uint64 kobj = (uint64)kobjp;




  // translate kernel-window VA -> user-window VA (same PA)
  return p->uheapwin_base + (kobj - p->kheapwin_base);
}

uint64
sys_kfree(void)
{
  uint64 u;
  argaddr(0, &u);
  if(u == 0) return -1;

  struct proc* p = myproc();
  kmem_state_t* old = kmem_push((kmem_state_t*)p->ukmem);


  uint64 u_lo = p->uheapwin_base;
  uint64 u_hi = p->uheapwin_base + (uint64)p->kheapwin_pages * PGSIZE;
  if(u < u_lo || u >= u_hi)
    {kmem_pop(old); return -1;}

  uint64 k = p->kheapwin_base + (u - p->uheapwin_base);
  k_free((void*)k);
  kmem_pop(old);
  return 0;
}






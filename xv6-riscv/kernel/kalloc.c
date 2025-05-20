// Physical memory allocator, for user pages,
// kernel stacks, page table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: page control variables
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

// vm.c의 락들
extern struct { struct spinlock lock; } page_lock;
extern struct { struct spinlock lock; } lru_lock;
extern struct { struct spinlock lock; } swap_bitmap_lock;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&page_lock.lock, "page");
  initlock(&lru_lock.lock, "lru");
  initlock(&swap_bitmap_lock.lock, "swapbitmap");
  init_swapbitmap();  // 스왑 비트맵 초기화

  // pages[] 배열의 필드들을 명시적으로 초기화
  for(int i = 0; i < PHYSTOP/PGSIZE; i++) {
    pages[i].in_lru = 0;
    pages[i].is_page_table = 0;
    pages[i].vaddr = 0;
  }

  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
#include "defs.h"      // select_victim, evictpage 프로토타입

void *
kalloc(void)
{
  struct run *r;

retry:
  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    release(&kmem.lock);
    memset((char*)r, 5, PGSIZE); // fill with junk
    return (void*)r;
  }
  release(&kmem.lock);

  // freelist가 비어있으면 스왑 아웃 시도
  // printf("[KALLOC] Free list empty, attempting to evict a page\n");
  if(evictpage()) {
    // printf("[KALLOC] Page eviction successful, retrying allocation\n");
    goto retry;  // 스왑 성공했으면 다시 시도
  }

  // printf("[KALLOC] Page eviction failed\n");
  return 0;
}

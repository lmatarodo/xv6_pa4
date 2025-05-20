#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"

// 스왑 관련 전역 변수
#define MAX_SWAP_PAGES (SWAPMAX / PGSIZE)
#define LRU_LOCKED 1   // 편의 매크로

static char swap_bitmap[MAX_SWAP_PAGES]; // 0: free, 1: used
struct { struct spinlock lock; } swap_bitmap_lock;  // 스왑 비트맵 보호를 위한 락
struct { struct spinlock lock; } pte_lock;  // PTE 업데이트 보호를 위한 락

// 스왑 통계를 위한 전역 변수
int swap_out_count = 0;  // 스왑 아웃 횟수
int swap_in_count = 0;   // 스왑 인 횟수
struct { struct spinlock lock; } swap_stats_lock;  // 통계 보호를 위한 락

// LRU 리스트 전역 변수
struct page *lru_head = NULL;   // 가장 오래된(victim 후보)
struct page *lru_tail = NULL;   // 가장 최근에 접근된 페이지
struct { struct spinlock lock; } lru_lock;  // LRU 리스트 보호를 위한 락
struct { struct spinlock lock; } page_lock;  // pages[] 배열 보호를 위한 락

// kalloc.c의 전역 변수 참조
extern int num_lru_pages;

// LRU 리스트 일관성 검사 함수
void check_lru_consistency() {
  // 락 획득 순서: page_lock -> lru_lock
  acquire(&page_lock.lock);
  acquire(&lru_lock.lock);

  int count = 0;
  struct page *p = lru_head;
  if (!p) {
    if (num_lru_pages != 0)
      // printf("check_lru: mismatch! num_lru_pages=%d but list empty\n", num_lru_pages);
    release(&lru_lock.lock);
    release(&page_lock.lock);
    return;
  }
  do {
    count++;
    if (count > 100000) {
      // printf("check_lru: infinite loop detected\n");
      break;
    }
    p = p->next;
  } while(p && p != lru_head);
  if (count != num_lru_pages) {
    // printf("check_lru: mismatch! counted=%d, recorded=%d\n", count, num_lru_pages);
  }

  // 락 해제 순서: lru_lock -> page_lock
  release(&lru_lock.lock);
  release(&page_lock.lock);
}

// LRU 노드 삽입
void
lru_add(struct page *p, pagetable_t pagetable, uint64 vaddr, int use_lock)
{
  // 페이지 포인터 유효성 검사
  if (p < &pages[0] || p >= &pages[PHYSTOP/PGSIZE]) {
    // printf("[LRU ADD] Invalid page pointer: %p\n", p);
    return;
  }

  // 가상 주소 유효성 검사
  if ((uint64)vaddr >= MAXVA) {
    // printf("[LRU ADD] Invalid vaddr: 0x%lx\n", (uint64)vaddr);
    return;
  }

  // 페이지 테이블 페이지 체크
  if (p->is_page_table) {
    // printf("[LRU] Warning: tried to add page table page to LRU (va=0x%lx)\n", vaddr);
    return;
  }

  // 이중 체크: 페이지 테이블이 아니고, 가상 주소가 유효한 경우에만 진행
  if (p->is_page_table || vaddr >= MAXVA) {
    // printf("[LRU] Double check failed: is_page_table=%d, vaddr=0x%lx\n", p->is_page_table, vaddr);
    return;
  }

  // 락 획득 순서: page_lock -> lru_lock
  if (use_lock) {
    acquire(&page_lock.lock);
    acquire(&lru_lock.lock);
  }

  // 메타데이터 설정
  p->pagetable = pagetable;
  p->vaddr = (char*)vaddr;

  // 원래 리스트에 있었는지 여부를 먼저 판단
  int was_in_list = p->in_lru;

  // 이미 리스트에 있었으면 제거
  if (was_in_list) {
    // p가 head이면서 next NULL인 경우 빈 리스트일 때 예외 처리
    if (lru_head == p && p->next == NULL && p->prev == NULL) {
      ; // 빈 리스트 그대로
    } else {
      // 리스트 중간/끝에 있으면 제거
      if (p->prev) p->prev->next = p->next;
      if (p->next) p->next->prev = p->prev;
      if (lru_head == p)    lru_head = p->next;
      if (lru_tail == p)    lru_tail = p->prev;
      p->prev = p->next = NULL;  // 리스트에서 완전히 분리
      p->in_lru = 0;  // 리스트에서 제거됨을 표시
      num_lru_pages--;  // 기존에 있던 노드 제거
    }
  }

  // 빈 리스트일 때
  if (!lru_head) {
    lru_head = lru_tail = p;
    p->next = p->prev = p;  // 자기 자신을 가리키도록
    p->in_lru = 1;  // 리스트에 추가됨을 표시
    num_lru_pages = 1;  // 첫 페이지 추가
  } else {
    // 리스트 끝에 붙이기 (원형 리스트)
    p->next = lru_head;     // 끝->head 연결
    p->prev = lru_tail;     // 끝->이전 tail 연결
    lru_head->prev = p;     // head->새 tail 연결
    lru_tail->next = p;     // 이전 tail->새 tail 연결
    lru_tail = p;           // tail 업데이트
    p->in_lru = 1;  // 리스트에 추가됨을 표시
    if (!was_in_list) {  // 새로 추가되는 경우에만 카운트 증가
      num_lru_pages++;  // page_lock과 lru_lock으로 보호됨
    }
  }

  // 락 해제 순서: lru_lock -> page_lock
  if (use_lock) {
    release(&lru_lock.lock);
    release(&page_lock.lock);
  }
  
  // 일관성 검사 (락을 다시 획득)
  if (use_lock) {
    check_lru_consistency();
  }
}

// LRU 노드 제거
void
lru_remove(struct page *p, int use_lock)
{
  // 락 획득 순서: page_lock -> lru_lock
  if (use_lock) {
    acquire(&page_lock.lock);
    acquire(&lru_lock.lock);
  }

  // 실제 리스트에서 제거되는 경우만 카운트 감소
  if (p->in_lru) {
    if (lru_head == p && lru_head == lru_tail) { // 마지막 노드
      lru_head = lru_tail = NULL;
    } else {
      if (lru_head == p)  lru_head = p->next;
      if (lru_tail == p)  lru_tail = p->prev;
      if (lru_head)  lru_head->prev = lru_tail;
      if (lru_tail)  lru_tail->next = lru_head;
    }
    p->prev = p->next = NULL;  // 리스트에서 완전히 분리
    p->in_lru = 0;  // 리스트에서 제거됨을 표시
    p->vaddr = 0;   // vaddr도 초기화
    num_lru_pages--;  // page_lock과 lru_lock으로 보호됨
  }

  // 락 해제 순서: lru_lock -> page_lock
  if (use_lock) {
    release(&lru_lock.lock);
    release(&page_lock.lock);
  }
  
  // 일관성 검사 (락을 다시 획득)
  if (use_lock) {
    check_lru_consistency();
  }
}

// 스왑 비트맵 초기화
void
init_swapbitmap(void)
{
  for (int i = 0; i < MAX_SWAP_PAGES; i++)
    swap_bitmap[i] = 0;
}

// 스왑 공간 할당
int
allocswap(void)
{
  acquire(&swap_bitmap_lock.lock);
  for (int i = 0; i < MAX_SWAP_PAGES; i++) {
    if (swap_bitmap[i] == 0) {
      swap_bitmap[i] = 1;
      release(&swap_bitmap_lock.lock);
      return i; // 페이지 단위 blkno 반환
    }
  }
  release(&swap_bitmap_lock.lock);
  panic("allocswap: out of swap space");  // 스왑 공간 부족 시 panic
  return -1; // unreachable
}

// 스왑 공간 해제
void
freeswap(int blkno)
{
  if (blkno < 0 || blkno >= MAX_SWAP_PAGES)
    panic("freeswap: invalid blkno");

  acquire(&swap_bitmap_lock.lock);
  swap_bitmap[blkno] = 0;
  release(&swap_bitmap_lock.lock);
}

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  //printf("[DEBUG] walk: va = 0x%lx, alloc = %d\n", va, alloc);
  
  if(va >= MAXVA) {
    printf("[WALK] va out of range: 0x%lx\n", va);
    panic("walk");
  }

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    //printf("[DEBUG] walk: level %d, pte = 0x%lx\n", level, *pte);
    
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      
      // 새로 할당된 페이지는 page table 용도이므로 플래그 설정
      uint64 phys = (uint64)pagetable - KERNBASE;  // 커널 가상 주소를 물리 주소로 변환
      struct page *pg = &pages[phys / PGSIZE];
      pg->is_page_table = 1;
      pg->vaddr = 0;  // 페이지 테이블 페이지는 가상 주소가 없음
      // printf("[WALK] Allocated page table page: pa=0x%lx\n", phys);
      
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (!pte) return 0;
  // swap-in case
  if (!(*pte & PTE_V) && (*pte & PTE_SWAP)) {
    // printf("[SWAPIN] va: 0x%lx, pte: 0x%lx\n", va, *pte);
    int blkno = PTE2PPN(*pte);
    char *mem = kalloc(); // 새 물리 페이지 할당
    if (!mem) return 0;
    swapread((uint64)mem, blkno); // swap에서 읽어오기
    freeswap(blkno);  // 사용이 끝난 swap 공간 반환
    
    // 스왑 인 통계 업데이트
    acquire(&swap_stats_lock.lock);
    swap_in_count++;
    // printf("[SWAP IN] va: 0x%lx, blk: %d, mem: 0x%lx (total: %d)\n", va, blkno, (uint64)mem, swap_in_count);
    release(&swap_stats_lock.lock);
    
    *pte = PA2PTE(mem) | (PTE_FLAGS(*pte) & ~PTE_SWAP) | PTE_V; // PPN 갱신 + SWAP 제거
    // printf("[SWAPIN] Updated PTE: 0x%lx\n", *pte);
    sfence_vma();
    // add to LRU
    struct page *pg = &pages[(uint64)mem / PGSIZE];
    if (!pg->in_lru && !pg->is_page_table && va != 0 && va < MAXVA) {
      lru_add(pg, pagetable, va, 1);
    }
    return (uint64)mem;
  }
  if (!(*pte & PTE_V) || !(*pte & PTE_U)) return 0;
  return PTE2PA(*pte);
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    
    // printf("[MAPPAGES] va=0x%lx pa=0x%lx perm=0x%x (R=%d W=%d X=%d U=%d)\n", 
    //        a, pa, perm,
    //        (perm & PTE_R) != 0,
    //        (perm & PTE_W) != 0,
    //        (perm & PTE_X) != 0,
    //        (perm & PTE_U) != 0);
    
    acquire(&pte_lock.lock);
    *pte = PA2PTE(pa) | perm | PTE_V;
    sfence_vma();
    release(&pte_lock.lock);
    
    if (perm & PTE_U) { // 사용자 페이지만 스왑 대상
      struct page *pg = &pages[pa/PGSIZE];
      if (!pg->in_lru && !pg->is_page_table && a < MAXVA) {
        // printf("[MAP] Adding page to LRU: pa=0x%lx, va=0x%lx\n", pa, a);
        lru_add(pg, pagetable, a, LRU_LOCKED);
      }
    }   
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      struct page *pg = &pages[pa / PGSIZE];
      if (pg->vaddr == 0) {
        // printf("[UNMAP] Warning: page at pa=0x%lx has null vaddr\n", pa);
      } else if ((uint64)pg->vaddr >= MAXVA) {
        // printf("[UNMAP] Warning: page at pa=0x%lx has invalid vaddr=0x%lx\n", pa, (uint64)pg->vaddr);
      }
      // 실제 LRU에 들어 있는 경우에만 제거
      if (pg->in_lru) {
        lru_remove(pg, 1);
        pg->vaddr = 0;    // 깔끔하게 vaddr도 초기화
      }
      kfree((void*)pa);
    } else if (*pte & PTE_SWAP) {
      // free swap block
      int blkno = PTE2PPN(*pte);
      freeswap(blkno); // 스왑 페이지 비트맵 클리어
    }
    acquire(&pte_lock.lock);
    *pte = 0;
    sfence_vma();
    release(&pte_lock.lock);
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  struct page *pg = &pages[(uint64)mem/PGSIZE];
  // user 코드 페이지(va=0)는 예외적으로 허용
  if (!pg->is_page_table) {
    // printf("[FIRST] Adding first page to LRU: pa=0x%lx, va=0x0\n", (uint64)mem);
    lru_add(pg, pagetable, 0, 0);
  }
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    // printf("[UVMMALLOC] Mapping page at va=0x%lx\n", a);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_W|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  // 페이지 테이블 페이지 해제 시 플래그 초기화
  struct page *pg = &pages[((uint64)pagetable - KERNBASE) / PGSIZE];
  pg->is_page_table = 0;
  pg->vaddr = 0;
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    
    // 스왑된 페이지 처리
    if((*pte & PTE_V) == 0 && (*pte & PTE_SWAP)) {
      // printf("[COPY] Found swapped page at va: 0x%lx, pte: 0x%lx\n", i, *pte);
      // 1. 새 물리 페이지 할당
      if((mem = kalloc()) == 0)
        goto err;
      
      // 2. 스왑에서 읽어오기
      int blkno = PTE2PPN(*pte);
      swapread((uint64)mem, blkno);
      // printf("[COPY] Swapped page read: va: 0x%lx, blk: %d, mem: 0x%lx\n", i, blkno, (uint64)mem);
      
      // 3. 자식에 매핑 (부모 PTE는 그대로 둠)
      flags = PTE_FLAGS(*pte) & (PTE_R|PTE_W|PTE_X|PTE_U);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){ 
        kfree(mem);
        goto err;
      }
      continue;
    }
    
    // 일반 페이지 처리 (기존 로직)
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    
    // 원본 페이지의 vaddr 검사
    struct page *old_pg = &pages[pa/PGSIZE];
    if (old_pg->vaddr == 0) {
      // printf("[COPY] Warning: source page at pa=0x%lx has null vaddr\n", pa);
    } else if ((uint64)old_pg->vaddr >= MAXVA) {
      // printf("[COPY] Warning: source page at pa=0x%lx has invalid vaddr=0x%lx\n", pa, (uint64)old_pg->vaddr);
    }
    
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
       (*pte & PTE_W) == 0)
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// Clock 알고리즘을 위한 전역 변수
static struct page *clock_hand = 0;

// Choose a victim page using Clock algorithm
// Returns pointer to struct page, or NULL if none
struct page*
select_victim(void)
{
  // 락 획득 순서: page_lock -> lru_lock
  acquire(&page_lock.lock);
  acquire(&lru_lock.lock);

  // LRU 리스트가 비어 있으면 실패
  if (!lru_head) {
    // printf("[CLOCK] LRU list is empty\n");
    release(&lru_lock.lock);
    release(&page_lock.lock);
    return 0;
  }

  if (clock_hand == 0) {
    clock_hand = lru_head;
    // printf("[CLOCK] Initialized clock_hand to head: 0x%lx\n", (uint64)clock_hand->vaddr);
  }

  // clock_hand의 가상 주소 유효성 검사
  if ((uint64)clock_hand->vaddr >= MAXVA) {
    // printf("[CLOCK] clock_hand has invalid vaddr: 0x%lx\n", (uint64)clock_hand->vaddr);
    panic("clock_hand vaddr invalid");
  }

  struct page *start = clock_hand;
  // printf("[CLOCK] Starting scan from: 0x%lx\n", (uint64)start->vaddr);
  
  while (1) {
    // printf("[DEBUG] victim vaddr = 0x%lx\n", (uint64)clock_hand->vaddr);
    // printf("[DEBUG] walking va = 0x%lx\n", (uint64)clock_hand->vaddr);
    pte_t *pte = walk(clock_hand->pagetable, (uint64)clock_hand->vaddr, 0);
    // printf("[CLOCK] checking page vaddr: 0x%lx (pte: 0x%lx)\n", (uint64)clock_hand->vaddr, pte ? *pte : 0);

    /* 1) 이미 스왑됐거나, 커널/고정 영역이면 건너뜀 */
    if (!pte || !(*pte & PTE_V) || 
        (uint64)clock_hand->vaddr >= KERNBASE ||
        (uint64)clock_hand->vaddr >= TRAMPOLINE) { 
      // printf("[CLOCK] skipping page: invalid or kernel/trampoline\n");
      clock_hand = clock_hand->next;
    }
    /* 2) 아직 참조(A) 비트가 살아 있으면 지우고 tail로 보냄 */
    else if (*pte & PTE_A) {
      // printf("[CLOCK] page has A bit set, clearing and moving to tail\n");
      *pte &= ~PTE_A;  // 참조 비트 클리어
      
      // 현재 페이지를 tail로 이동
      struct page *p = clock_hand;
      clock_hand = clock_hand->next;  // 다음 페이지로 이동
      
      // 페이지가 이미 tail이면 이동할 필요 없음
      if (p != lru_tail) {
        // 리스트에서 제거 (이미 락을 획득했으므로 use_lock=0)
        lru_remove(p, 0);
        // tail에 추가 (이미 락을 획득했으므로 use_lock=0)
        lru_add(p, p->pagetable, (uint64)p->vaddr, 0);
      }
    }
    /* 3) A == 0 이면 victim 확정 */
    else {
      // printf("[CLOCK] found victim: vaddr: 0x%lx\n", (uint64)clock_hand->vaddr);
      struct page *victim = clock_hand;
      clock_hand = clock_hand->next;
      release(&lru_lock.lock);
      release(&page_lock.lock);
      return victim;
    }

    /* 리스트 한 바퀴를 돌았는데도 못 찾으면 그냥 현재 손가락 */
    if (clock_hand == start) {
      // printf("[CLOCK] clock_hand returned to start, using current page as victim\n");
      struct page *victim = clock_hand;
      clock_hand = clock_hand->next;
      release(&lru_lock.lock);
      release(&page_lock.lock);
      return victim;
    }
  }
}

// Evict one page: swap out to disk, update PTE, free physical page
// Returns 1 on success, 0 on failure
int
evictpage(void)
{
  struct page *victim = select_victim();
  if (!victim)
    return 0;                       // LRU 비어 있음

  // victim의 필드들을 안전하게 복사
  pagetable_t victim_pagetable = victim->pagetable;
  uint64 victim_vaddr = (uint64)victim->vaddr;

  // victim의 가상 주소 유효성 검사
  if ((uint64)victim_vaddr >= MAXVA) {
    // printf("[EVICT] Invalid victim vaddr: 0x%lx\n", victim_vaddr);
    panic("invalid victim vaddr");
  }

  // printf("[DEBUG] evictpage: victim_vaddr = 0x%lx\n", victim_vaddr);
  // printf("[DEBUG] evictpage: victim_pagetable = %p\n", victim_pagetable);

  /* victim의 PTE 찾기 */
  // printf("[DEBUG] evictpage: walking va = 0x%lx\n", victim_vaddr);
  pte_t *pte = walk(victim_pagetable, (uint64)victim_vaddr, 0);
  if (!pte || !(*pte & PTE_V))
    return 0;
  uint64 pa = PTE2PA(*pte);

  // printf("[EVICT] va: 0x%lx, pa: 0x%lx, pte: 0x%lx\n", victim_vaddr, pa, *pte);

  /* 1. 빈 swap 슬롯 할당 */
  int blkno = allocswap();  // 이제 panic이 발생할 수 있음
  if (blkno < 0)  // unreachable
    return 0;                       // swap 공간 부족

  /* 2. 디스크로 write-out */
  swapwrite(pa, blkno);
  
  // 스왑 아웃 통계 업데이트
  acquire(&swap_stats_lock.lock);
  swap_out_count++;
  // printf("[SWAP OUT] pa = 0x%lx → blkno = %d (total: %d)\n", pa, blkno, swap_out_count);
  release(&swap_stats_lock.lock);

  /* 3. LRU 리스트에서 제거 (이미 락을 획득했으므로 use_lock=0) */
  lru_remove(victim, LRU_LOCKED);

  /* 4. PTE 업데이트: V 비트 끄고 SWAP 슬롯 번호 저장 */
  uint64 flags = PTE_FLAGS(*pte) & (PTE_R|PTE_W|PTE_X|PTE_U);
  acquire(&pte_lock.lock);
  *pte = (blkno << 12) | (flags & ~PTE_V) | PTE_SWAP;
  sfence_vma();
  release(&pte_lock.lock);
  // printf("[EVICT] Updated PTE: 0x%lx\n", *pte);

  /* 5. 물리 페이지 free */
  kfree((void*)pa);

  /* 6. struct page 메타데이터 초기화 */
  struct page *pg = &pages[pa/PGSIZE];
  pg->pagetable = 0;
  pg->vaddr = 0;
  pg->in_lru = 0;
  pg->is_page_table = 0;
  // printf("[EVICT] Cleared page metadata for pa=0x%lx\n", pa);

  return 1;
}

// 스왑 통계 출력
void
print_swap_stats(void)
{
  acquire(&swap_stats_lock.lock);
  // printf("Swap Statistics:\n");
  // printf("  Swap Out: %d pages\n", swap_out_count);
  // printf("  Swap In: %d pages\n", swap_in_count);
  // printf("  Total Swaps: %d pages\n", swap_out_count + swap_in_count);
  release(&swap_stats_lock.lock);
}